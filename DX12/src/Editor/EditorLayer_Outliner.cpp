// The Hierarchy / Outliner panel of EditorLayer, and ExecuteEditorCommand (the
// control channel's editor ops). Member definitions plus TU-local helpers and
// deferred-op state (s_pending*, s_hierView, ...) used only by this panel.
// Shared bits (kWindowHierarchy, s_faIconsLoaded, SaveFileDialog) live in
// Editor/EditorLayerShared.h.
#include "Editor/EditorLayer.h"
#include "Editor/AttackEditor.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/EditorLayerShared.h"        // kWindowHierarchy, s_faIconsLoaded, SaveFileDialog
#include "Editor/FontAwesomeIcons.h"
#include "Editor/UIEditorWindow.h"           // a UI row: Edit in UI Editor
#include "Editor/UITemplates.h"              // GuidStampAction (MoveUINode's undo)
#include "Editor/Undo/ValueEditAction.h"
#include "Editor/Undo/RenameAction.h"        // RenameAction
#include "Editor/Undo/StructuralAction.h"    // StructuralAction/ReparentAction + EntitySubtreeSnapshot
#include "ECS/AnimationComponents.h"         // SocketComponent (Attach to Socket)
#include "ECS/Components.h"                  // CameraComponent, LightData, EditorCameraTag, ...
#include "ECS/HierarchyComponents.h"         // LocalTransform, GlobalTransform, Parent, Children, MeshLibRef, etc.
#include "ECS/StaticMeshComponents.h"        // MeshSectionOf - derived sections are hidden rows by default
#include "ECS/EditorFolderComponent.h"       // editor outliner folder membership
#include "ECS/FollowComponents.h"            // FollowSocket/FollowEntity (logical hierarchy edges)
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "ECS/CameraStackComponents.h"       // VirtualCameraComponent + CameraStackChannelSingleton
#include "ECS/PhysicsComponents.h"           // ColliderComponent ("Wrap Visual as Child" Y lift)
#include "ECS/TerrainComponent.h"            // per-type outliner icons (HierarchyTypeIcon)
#include "ECS/WaterComponent.h"
#include "ECS/GrassComponent.h"
#include "ECS/ParticleComponent.h"
#include "ECS/BillboardFXComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/WorldEnvironment.h"
#include "ECS/TODComponents.h"
#include "ECS/DDGIComponents.h"
#include "ECS/ReflectionProbeComponent.h"
#include "UI/UICanvas.h"                     // UI::UIParent (canvas-UI hierarchy edges)
#include "UI/UICanvasLayout.h"               // UI::ResolveUIParent / UIChildrenOf
#include "UI/UIDocument.h"                   // UI::UIDocumentMember: an instance is not rows
#include "Resource/PrefabSerializer.h"       // SavePrefab ("Save as Prefab") / LoadPrefab (.ipfb drop)
#include "Scene/SceneInstanceLoader.h"       // WriteMaterialBindings ("Write Materials to Source Model")
#include "System/PathUtil.h"                 // Sys::RelativizeAssetPath
#include "System/Log.h"
#include "Harness/ControlChannelJobs.h"   // the `save` op answers a jobId

#include "nlohmann/json.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"            // BeginDragDropTargetCustom, ImGuiWindow::InnerRect

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // ---- Hierarchy panel state: reveal, inline rename, the tree walk's deferred ops ----
    Entity s_revealEntity = NullEntity;
    std::unordered_set<Entity> s_revealAncestors;
    std::string s_revealFolder;
    Entity s_renamingEntity      = NullEntity;
    bool   s_renameWantsFocus    = false;   // give the rename field the keyboard once
    char   s_renameBuffer[256]   = {};
    Entity s_pendingDelete       = NullEntity;
    Entity s_pendingCreateChild  = NullEntity;
    Entity s_pendingDuplicate    = NullEntity;
    Entity s_pendingPrefabSave   = NullEntity;
    Entity s_pendingWrapVisual   = NullEntity;       // context-menu -> wrap visual into child
    Entity s_pendingIscnMatWrite = NullEntity;       // context-menu -> write mats back to .iscn
    Entity s_pendingUnpackModel  = NullEntity;       // context-menu -> Unpack Model Instance
    // Drag-drop reparent. child == NullEntity means "no pending"; newParent
    // == NullEntity means "drop into root (unparent)".
    struct PendingReparent { Entity child = NullEntity; Entity newParent = NullEntity; };
    PendingReparent s_pendingReparent;
    // A UI row dropped on the top / bottom quarter of another UI row: before /
    // after it among that row's siblings.
    struct PendingUIReorder { Entity child = NullEntity; Entity anchor = NullEntity; bool after = false; };
    PendingUIReorder s_pendingUIReorder;
    // Context menu > Attach to Socket > owner > socket.
    struct PendingAttach { Entity item = NullEntity; Entity owner = NullEntity; std::string socket; };
    PendingAttach s_pendingAttach;
    // Why the attach would refuse the row's entity whatever the socket
    // (EditorLayer::AttachRefusal): the menu shows it instead of a click that only
    // logs. Set by RenderHierarchyPanel for its tree walk.
    std::function<std::string(Entity)> s_attachRefusal;

    // Rename commit, deferred so RenderHierarchyPanel (a member) can record the
    // undo entry; DrawEntityTree has already applied the SetName.
    struct PendingRename { Entity entity = NullEntity; std::string oldName; std::string newName; };
    PendingRename s_pendingRename;

    // True when @p a is @p e or one of its ancestors (the Parent chain).
    bool IsSelfOrAncestor(World& w, Entity a, Entity e)
    {
        for (int guard = 0; e != NullEntity && guard < 256; ++guard)
        {
            if (e == a) return true;
            const Parent* p = w.GetComponent<Parent>(e);
            e = p ? p->entity : NullEntity;
        }
        return false;
    }

    // Every entity with sockets @p item may follow (not itself, not one of its
    // ancestors - EditorLayer::NearestModelInstance's rule - not one of its
    // descendants, whose sockets move with it; never a preview stage), each a
    // submenu of its sockets. A click queues the attach (applied after the tree).
    void DrawAttachToSocketMenu(World& w, Entity item)
    {
        auto* pool = w.GetPool<SocketComponent>();
        int shown = 0, more = 0;
        if (pool)
        {
            const auto& ents = pool->Entities();
            const auto& data = pool->Data();
            for (size_t i = 0; i < ents.size(); ++i)
            {
                const Entity o = ents[i];
                const SocketComponent& sc = data[i];
                if (sc.count == 0 || !w.IsAlive(o) || w.HasComponent<PreviewStageTag>(o)) continue;
                if (IsSelfOrAncestor(w, o, item) || IsSelfOrAncestor(w, item, o)) continue;
                // An owner that follows the item (a sword on its hand): they would follow
                // each other.
                if (EditorLayer::FollowsEntity(w, o, item)) continue;
                if (shown >= 64) { ++more; continue; }
                ++shown;
                char label[160];
                std::snprintf(label, sizeof(label), "%s##sockown%u", w.GetName(o).c_str(), static_cast<unsigned>(o));
                if (!ImGui::BeginMenu(label)) continue;
                for (uint32_t k = 0; k < sc.count; ++k)
                {
                    const SocketComponent::Socket& s = sc.sockets[k];
                    const std::string name(s.name, strnlen(s.name, sizeof(s.name)));
                    const std::string where = (s.flags & SocketComponent::kMeshSocket)
                        ? std::string("mesh") : std::string(s.boneName, strnlen(s.boneName, sizeof(s.boneName)));
                    ImGui::PushID(static_cast<int>(k));
                    if (ImGui::MenuItem(name.c_str(), where.c_str()))
                        s_pendingAttach = { item, o, name };
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
        }
        if (shown == 0) ImGui::TextDisabled("No other entity has sockets");
        if (more > 0) ImGui::TextDisabled("(%d more)", more);
    }

    // ---- Editor outliner folders (Unreal-style), deferred ops ----
    std::string s_renamingFolder;                 // full path being renamed ("" = none)
    char        s_folderRenameBuffer[256] = {};
    // Assign (or clear, path == "") an entity's outliner folder path.
    struct PendingFolderSet { Entity entity = NullEntity; std::string path; bool valid = false; std::vector<Entity> entities; bool direct = false; };
    PendingFolderSet s_pendingFolderSet;
    // Folder-level create / rename / delete / move (processed after the walk).
    struct PendingFolderOp {
        enum Kind { None, Create, Rename, Delete, MoveFolder } kind = None;
        std::string path;   // target / source folder full-path
        std::string arg;    // new leaf name (create/rename) or new parent path (move)
    };
    PendingFolderOp s_pendingFolderOp;

    // ---- Dynamic hierarchy view ----
    // Runtime ECS is flat: relations are Parent / Children, FollowSocketComponent /
    // FollowEntityComponent and, for Canvas UI, UIParent. The panel derives a tree
    // from them, so a satellite entity (HPBar, NameTag, weapon) following its
    // target appears under that target though no Parent links them. Priority when
    // an entity has several connectors (the first non-null wins):
    //   UIParent  >  Parent  >  FollowSocket  >  FollowEntity
    //
    // Stored dense, indexed by entity id, so a rebuild refills buffers it already
    // owns and allocates nothing once warm (node-based maps cost ~59k allocations
    // and a 4.9 ms spike per rebuild at 15k entities). Children live in one
    // flattened pool addressed CSR-style.
    struct HierarchyMap
    {
        std::vector<Entity>        parentOf;    // [id] -> logical parent (NullEntity if root/dead)
        std::vector<std::uint8_t>  cyclicFlag;  // [id] -> 1 if this entity's parent link was severed
        std::vector<std::uint32_t> childBegin;  // [id] -> first child index in childPool
        std::vector<std::uint32_t> childCount;  // [id] -> number of children
        std::vector<Entity>        childPool;   // flattened child runs, each ascending by id
        std::vector<Entity>        aliveIds;    // ascending; the entities this build covered
        std::vector<std::uint32_t> fillCursor;  // scratch for the counting-sort scatter

        Entity ParentOf(Entity e) const
        {
            return e < parentOf.size() ? parentOf[e] : NullEntity;
        }
        bool IsCyclic(Entity e) const
        {
            return e < cyclicFlag.size() && cyclicFlag[e] != 0;
        }
        // Returns nullptr with outCount == 0 when e has no logical children.
        const Entity* ChildrenOf(Entity e, std::uint32_t& outCount) const
        {
            if (e >= childCount.size() || childCount[e] == 0) { outCount = 0; return nullptr; }
            outCount = childCount[e];
            return childPool.data() + childBegin[e];
        }
    };

    // Component pools consulted per entity, fetched once per BuildHierarchyMap
    // instead of 2-4 GetComponent type_index lookups per entity.
    struct LogicalParentPools
    {
        const ComponentPool<UI::UIParent>*          uiParent;
        const ComponentPool<Parent>*                parent;
        const ComponentPool<FollowSocketComponent>* followSocket;
        const ComponentPool<FollowEntityComponent>* followEntity;

        explicit LogicalParentPools(World* w)
            : uiParent    (w->GetPool<UI::UIParent>())
            , parent      (w->GetPool<Parent>())
            , followSocket(w->GetPool<FollowSocketComponent>())
            , followEntity(w->GetPool<FollowEntityComponent>())
        {}
    };

    Entity ResolveFollowTarget(World* world, const AttachmentRef& ref)
    {
        return ref.Resolve(*world);
    }

    Entity LogicalParentOf(World* world, const LogicalParentPools& pools, Entity e)
    {
        // Entity-as-widget Canvas UI: show UI nodes under their UIParent (the
        // canvas or a parent node), so the canvas subtree appears as real
        // child entities in the Hierarchy panel.
        if (const auto* up = pools.uiParent ? pools.uiParent->Get(e) : nullptr)
        {
            Entity p = up->parent;
            if ((p == NullEntity || !world->IsAlive(p)) && up->parentGuid.IsValid())
                p = ECS::GuidRegistry::Get().Find(up->parentGuid);
            if (p != NullEntity && p != e && world->IsAlive(p)) return p;
        }

        if (const auto* pc = pools.parent ? pools.parent->Get(e) : nullptr;
            pc && pc->entity != NullEntity)
            if (world->IsAlive(pc->entity)) return pc->entity;

        if (const auto* fs = pools.followSocket ? pools.followSocket->Get(e) : nullptr)
        {
            const Entity t = ResolveFollowTarget(world, fs->target);
            if (t != NullEntity && t != e && world->IsAlive(t)) return t;
        }

        if (const auto* fe = pools.followEntity ? pools.followEntity->Get(e) : nullptr)
        {
            const Entity t = ResolveFollowTarget(world, fe->target);
            if (t != NullEntity && t != e && world->IsAlive(t)) return t;
        }

        return NullEntity;
    }

    // Build the hierarchy map in place, O(N): one pass resolves logical parents,
    // one cycle-sever walk (each walk early-outs at the first revisit, so the
    // worst case is bounded by the longest follow chain), then a counting sort
    // into the CSR child pool. Fills `m`'s existing buffers, which keeps a
    // rebuild allocation-free after the first.
    void BuildHierarchyMap(World* world, HierarchyMap& m, bool hideMeshSections)
    {
        // Entity is a uint32_t id, so the bound always fits; the cast keeps the
        // index arithmetic below in one type.
        const std::uint32_t bound = static_cast<std::uint32_t>(world->EntityIdBound());

        m.parentOf  .assign(bound, NullEntity);
        m.cyclicFlag.assign(bound, std::uint8_t{ 0 });
        m.childBegin.assign(bound, 0u);
        m.childCount.assign(bound, 0u);
        m.aliveIds.clear();
        m.childPool.clear();

        const LogicalParentPools pools(world);
        // A StaticMesh owner's derived sections are rebuilt at will and never
        // saved; unless asked for, they are not rows at all (not children, and
        // - having no entry here - not roots either: the root pass skips them too).
        const auto* sectionPool = hideMeshSections ? world->GetPool<MeshSectionOf>() : nullptr;
        // A UI Document's instance is the .iui's, made at run time: its host is the
        // row (edit the asset in its own UI Editor tab).
        const auto* docMembers = world->GetPool<UI::UIDocumentMember>();
        for (Entity e : world->GetEntities())
        {
            if (!world->IsAlive(e) || e >= bound) continue;
            if (sectionPool && sectionPool->Has(e)) continue;
            if (docMembers && docMembers->Has(e)) continue;
            m.parentOf[e] = LogicalParentOf(world, pools, e);
            m.aliveIds.push_back(e);
        }
        // Ascending ids: the child runs below come out sorted without a per-parent
        // std::sort, and the cycle sever is deterministic.
        std::sort(m.aliveIds.begin(), m.aliveIds.end());

        // Cycle detection with a reusable visited-stamp array (entity-id indexed):
        // no per-entity set allocation. Stamp == current walk id means visited.
        static std::vector<uint32_t> s_visitStamp;
        static uint32_t              s_walkId = 0;
        if (s_visitStamp.size() < bound)
            s_visitStamp.resize(bound, 0u);
        for (Entity e : m.aliveIds)
        {
            const uint32_t walkId = ++s_walkId;
            Entity cur = e;
            while (cur != NullEntity && cur < bound)
            {
                if (s_visitStamp[cur] == walkId)
                {
                    m.cyclicFlag[e] = 1u;
                    m.parentOf[e]   = NullEntity;  // sever: the cyclic entity becomes a root
                    break;
                }
                s_visitStamp[cur] = walkId;
                cur = m.parentOf[cur];
            }
        }

        // Counting sort into the flattened child pool: count, prefix-sum into
        // childBegin, then scatter in ascending-id order so each run is sorted.
        std::size_t total = 0;
        for (Entity e : m.aliveIds)
        {
            const Entity p = m.parentOf[e];
            if (p != NullEntity && p < bound) { ++m.childCount[p]; ++total; }
        }
        std::uint32_t offset = 0;
        for (std::uint32_t id = 0; id < bound; ++id)
        {
            m.childBegin[id] = offset;
            offset += m.childCount[id];
        }
        m.childPool.resize(total);
        m.fillCursor.assign(m.childBegin.begin(), m.childBegin.end());
        for (Entity e : m.aliveIds)
        {
            const Entity p = m.parentOf[e];
            if (p != NullEntity && p < bound)
                m.childPool[m.fillCursor[p]++] = e;
        }

        // A UI node's children in paint order ((siblingIndex, id), what layout and
        // hit test use), not by id.
        if (const auto* rects = world->GetPool<UI::UIRect>())
        {
            static std::vector<std::pair<int, Entity>> s_keyed;
            for (Entity e : m.aliveIds)
            {
                const std::uint32_t n = m.childCount[e];
                if (n < 2 || !(rects->Has(e) || world->HasComponent<UI::UICanvas>(e))) continue;
                Entity* run = m.childPool.data() + m.childBegin[e];
                s_keyed.clear();
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    const UI::UIRect* r = rects->Get(run[i]);
                    s_keyed.emplace_back(r ? r->siblingIndex : 0, run[i]);
                }
                std::sort(s_keyed.begin(), s_keyed.end());
                for (std::uint32_t i = 0; i < n; ++i) run[i] = s_keyed[i].second;
            }
        }
    }

    // Recursively destroy an entity and its descendants (Children and UIParent
    // edges) and unlink it from its parent's Children.
    //
    // Near-twin of Hierarchy::DestroySubtree (ECS/HierarchyOps.h), the runtime
    // destroy path, and deliberately not replaced by it: this is the no-undo
    // branch of DeleteEntities, whose undo branch captures an
    // EntitySubtreeSnapshot, and both must agree on which entities form "the
    // subtree". Do not widen this one to follow PrefabInstanceMember (as the
    // runtime helper does) without teaching the snapshot the same edge: undo
    // would restore less than delete removed.
    void DestroyEntityRecursive(World* world, Entity e)
    {
        std::vector<Entity> children;
        if (const Children* ch = world->GetComponent<Children>(e))
            children = ch->entities;

        // Canvas-UI children are linked via UIParent, not the scene-graph Children
        // list: collect them too, so deleting a Canvas / UI node removes its whole UI
        // subtree instead of orphaning the child widgets.
        if (auto* pp = world->GetPool<UI::UIParent>())
        {
            const auto& ents = pp->Entities();
            const auto& data = pp->Data();
            for (size_t i = 0; i < data.size(); ++i)
                if (ents[i] != e && UI::ResolveUIParent(*world, data[i]) == e)   // GUID-only edges too
                    children.push_back(ents[i]);
        }

        for (Entity child : children)
            if (world->IsAlive(child))
                DestroyEntityRecursive(world, child);

        if (const Parent* p = world->GetComponent<Parent>(e); p && p->entity != NullEntity)
        {
            if (Children* pc = world->GetComponent<Children>(p->entity))
            {
                auto& vec = pc->entities;
                vec.erase(std::remove(vec.begin(), vec.end(), e), vec.end());
            }
        }

        world->DestroyEntity(e);
    }

    // Cycle check for drag-drop: dropping `child` onto `newParent` would form
    // a cycle if `child` is itself an ancestor of `newParent`. Walks up the
    // parent chain with a hard depth cap (4096 hops) so a degenerate ECS state
    // doesn't hang the editor.
    bool WouldCreateCycle(World* world, Entity child, Entity newParent)
    {
        Entity cur = newParent;
        for (int hops = 0; hops < 4096 && cur != NullEntity; ++hops)
        {
            if (cur == child) return true;
            // A Canvas UI node's parent is its UIParent edge (resolved as layout
            // resolves it), everything else's the transform Parent; without this a UI
            // cycle would pass the check.
            if (const Entity up = UI::ResolveUIParent(*world, cur); up != NullEntity) { cur = up; continue; }
            const Parent* p = world->GetComponent<Parent>(cur);
            cur = p ? p->entity : NullEntity;
        }
        return false;
    }

    // ---- Canvas UI moves ----
    // One UI move as one undo step: every node it touched, before and after (the
    // moved node's edge, and the siblingIndex of each sibling the move
    // renumbered). The edge is restored by its GUID (an entity id is not stable
    // across a delete / undo) and the id re-resolved from it.
    class UIMoveAction : public Editor::Undo::IEditAction
    {
    public:
        struct NodeState
        {
            EntityHandle h;
            bool         hasEdge = false;
            UI::UIParent edge;
            bool         hasRect = false;
            int          sibling = 0;
        };
        UIMoveAction(std::vector<NodeState> before, std::vector<NodeState> after, std::string label)
            : m_before(std::move(before)), m_after(std::move(after)), m_label(std::move(label)) {}
        void        Apply(Editor::Undo::EditContext& c) override  { Set(c, m_after); }
        void        Revert(Editor::Undo::EditContext& c) override { Set(c, m_before); }
        const char* Label() const override { return m_label.c_str(); }

        static NodeState Capture(World& w, Entity e)
        {
            NodeState n;
            n.h = w.MakeHandle(e);
            if (const UI::UIParent* up = w.GetComponent<UI::UIParent>(e)) { n.hasEdge = true; n.edge = *up; }
            if (const UI::UIRect* r = w.GetComponent<UI::UIRect>(e)) { n.hasRect = true; n.sibling = r->siblingIndex; }
            return n;
        }

    private:
        static void Set(Editor::Undo::EditContext& c, const std::vector<NodeState>& states)
        {
            if (!c.world) return;
            World& w = *c.world;
            for (const NodeState& n : states)
            {
                if (!w.IsHandleValid(n.h)) continue;
                const Entity e = n.h.entity;
                if (n.hasEdge)
                {
                    UI::UIParent edge = n.edge;
                    if (edge.parentGuid.IsValid())
                        edge.parent = ECS::GuidRegistry::Get().Find(edge.parentGuid);
                    if (UI::UIParent* up = w.GetComponent<UI::UIParent>(e)) *up = edge;
                    else w.AddComponent<UI::UIParent>(e, edge);
                }
                else if (w.GetComponent<UI::UIParent>(e))
                    w.RemoveComponent<UI::UIParent>(e);
                if (n.hasRect)
                    if (UI::UIRect* r = w.GetComponent<UI::UIRect>(e)) r->siblingIndex = n.sibling;
            }
        }

        std::vector<NodeState> m_before, m_after;
        std::string            m_label;
    };

    // Move a component value from src to dst. Captures by value first, so a
    // pointer that AddComponent could relocate (pool growth) is never
    // dereferenced. Returns true when a move happened.
    template <typename T>
    bool MoveComponentBetween(World* world, Entity src, Entity dst)
    {
        T* p = world->GetComponent<T>(src);
        if (!p) return false;
        T value = *p;
        world->RemoveComponent<T>(src);
        world->AddComponent<T>(dst, std::move(value));
        return true;
    }

    // Reparent `child` under `newParent` (or detach when newParent ==
    // NullEntity) while preserving the child's world transform. The
    // LocalTransform is recomputed so child.GlobalTransform on the next
    // Propagate matches its current world pose: newLT = childGT * inv(newParentGT).
    void ReparentEntity(World* world, Entity child, Entity newParent)
    {
        using namespace DirectX;

        // Canvas UI nodes are parented via UIParent, not the transform Parent:
        // reparent there and skip the transform path.
        if (auto* up = world->GetComponent<UI::UIParent>(child))
        {
            up->parent     = newParent;
            up->parentGuid = (newParent != NullEntity && world->IsAlive(newParent))
                           ? ECS::EnsureGuidOn(*world, newParent)
                           : ECS::Guid{};
            return;
        }

        // 1. Recompute LocalTransform to preserve world pose.
        const GlobalTransform* childGT = world->GetComponent<GlobalTransform>(child);
        LocalTransform*        lt      = world->GetComponent<LocalTransform>(child);
        if (childGT && lt)
        {
            XMMATRIX worldM = XMLoadFloat4x4(&childGT->matrix);
            XMMATRIX newLocalM;
            if (newParent == NullEntity)
            {
                newLocalM = worldM; // root: local == world
            }
            else if (const GlobalTransform* pGT =
                         world->GetComponent<GlobalTransform>(newParent))
            {
                XMVECTOR det;
                XMMATRIX pInv = XMMatrixInverse(&det, XMLoadFloat4x4(&pGT->matrix));
                // Degenerate parent matrix (zero scale, etc.): fall back to the
                // child's world matrix as its local one - valid, though the child
                // may visually snap.
                if (XMVectorGetX(det) == 0.0f) newLocalM = XMLoadFloat4x4(&childGT->matrix);
                else                           newLocalM = XMMatrixMultiply(worldM, pInv);
            }
            else
            {
                newLocalM = worldM;
            }
            XMVECTOR t, r, s;
            if (XMMatrixDecompose(&s, &r, &t, newLocalM))
            {
                XMStoreFloat3(&lt->translation, t);
                XMStoreFloat4(&lt->rotation,    r);
                XMStoreFloat3(&lt->scale,       s);
            }
        }

        // 2. Detach from old parent's Children list.
        Parent* oldP = world->GetComponent<Parent>(child);
        const Entity oldParent = oldP ? oldP->entity : NullEntity;
        if (oldParent != NullEntity)
        {
            if (Children* oldCh = world->GetComponent<Children>(oldParent))
            {
                auto& v = oldCh->entities;
                v.erase(std::remove(v.begin(), v.end(), child), v.end());
            }
        }

        // 3. Write new Parent or remove if reparenting to root.
        if (newParent == NullEntity)
        {
            if (oldP) world->RemoveComponent<Parent>(child);
        }
        else
        {
            if (oldP) oldP->entity = newParent;
            else      world->AddComponent<Parent>(child, Parent{ newParent });

            // 4. Insert into new parent's Children list.
            if (Children* newCh = world->GetComponent<Children>(newParent))
            {
                newCh->entities.push_back(child);
            }
            else
            {
                Children c; c.entities.push_back(child);
                world->AddComponent<Children>(newParent, std::move(c));
            }
        }
    }

    struct HierIcon { const char* icon; ImVec4 color; bool tint; };

    // Unreal-style outliner icon + soft row tint chosen from the entity's
    // dominant component (most-specific first). Lets each entity read at a
    // glance by type. Returns a generic dot with no tint for a plain entity.
    HierIcon HierarchyTypeIcon(World* w, Entity e)
    {
        using namespace FontAwesome;
        // ---- Scene singletons / global managers (match the pin order) ----
        if (w->HasComponent<WorldEnvironmentComponent>(e))           return { kGlobe,     ImVec4(0.55f, 0.78f, 1.00f, 1.f), true };
        if (w->HasComponent<AtmosphereComponent>(e) ||
            w->HasComponent<SkyboxComponent>(e))                     return { kCloud,     ImVec4(0.65f, 0.82f, 0.95f, 1.f), true };
        if (w->HasComponent<TODConfigComponent>(e))                  return { kClock,     ImVec4(0.95f, 0.80f, 0.45f, 1.f), true };
        if (w->HasComponent<IndirectLightingSettingsComponent>(e))   return { kGear,      ImVec4(0.55f, 0.85f, 0.80f, 1.f), true };
        // ---- Lights (tag before generic light) ----
        if (w->HasComponent<SunLightTag>(e))                         return { kSun,       ImVec4(1.00f, 0.85f, 0.45f, 1.f), true };
        if (w->HasComponent<MoonLightTag>(e))                        return { kMoon,      ImVec4(0.70f, 0.78f, 1.00f, 1.f), true };
        if (w->HasComponent<LightData>(e))                           return { kLightbulb, ImVec4(0.98f, 0.90f, 0.55f, 1.f), true };
        // ---- Camera (lens, VCam, or bare FPS-controller entities all count) ----
        if (w->HasComponent<CameraComponent>(e) ||
            w->HasComponent<VirtualCameraComponent>(e) ||
            w->HasComponent<CameraControllerComponent>(e))           return { kCamera,    ImVec4(0.55f, 0.85f, 0.95f, 1.f), true };
        // ---- Environment geometry ----
        if (w->HasComponent<TerrainComponent>(e))                    return { kMountain,  ImVec4(0.80f, 0.70f, 0.50f, 1.f), true };
        if (w->HasComponent<WaterComponent>(e))                      return { kDroplet,   ImVec4(0.50f, 0.75f, 0.95f, 1.f), true };
        if (w->HasComponent<GrassComponent>(e))                      return { kLeaf,      ImVec4(0.60f, 0.85f, 0.55f, 1.f), true };
        // ---- VFX + GI volumes ----
        if (w->HasComponent<ParticleEmitterComponent>(e) ||
            w->HasComponent<BillboardFXComponent>(e))                return { kFire,      ImVec4(1.00f, 0.70f, 0.45f, 1.f), true };
        if (w->HasComponent<DDGIVolumeComponent>(e) ||
            w->HasComponent<ReflectionProbeComponent>(e))            return { kCube,      ImVec4(0.75f, 0.65f, 0.95f, 1.f), true };
        // ---- Mesh (soft-green tint) ----
        if (w->HasComponent<MeshLibRef>(e))                          return { kCube,      ImVec4(0.75f, 0.88f, 0.75f, 1.f), true };
        // ---- Plain / transform-only entity ----
        return { kCircle, ImVec4(0.55f, 0.55f, 0.55f, 1.f), false };
    }

    // Resolved-icon cache: HierarchyTypeIcon probes up to 19 pools per row.
    // Filled lazily per visible row; cleared on every hierarchy-view rebuild (see
    // HierarchyViewCache), so icon staleness after a component add / remove is
    // bounded by the same safety net (kHierarchySafetyNetFrames).
    std::unordered_map<Entity, HierIcon> s_hierIconCache;

    HierIcon CachedHierarchyIcon(World* w, Entity e)
    {
        auto it = s_hierIconCache.find(e);
        if (it == s_hierIconCache.end())
            it = s_hierIconCache.emplace(e, HierarchyTypeIcon(w, e)).first;
        return it->second;
    }

    // Recursive tree-node draw for the Hierarchy panel. Walks the logical-children
    // map (Parent / Children, Follow* and UIParent edges) rather than the raw
    // Children component, so entities that follow a target appear under it.
    //
    // Selection is deferred until release, so dragging an already selected row
    // keeps the whole selection and dragging an unselected row leaves the
    // Inspector target intact. Row colour always reflects actual membership.
    const std::vector<Entity>* s_selection = nullptr;
    std::unordered_set<Entity> s_selectedRows;
    std::vector<Entity> s_rowOrder;
    Entity s_clickEntity = NullEntity;
    EditorLayer::SelectMode s_clickMode = EditorLayer::SelectMode::Replace;
    bool s_clickPending = false;
    Entity s_pressedEntity = NullEntity;
    EditorLayer::SelectMode s_pressedMode = EditorLayer::SelectMode::Replace;
    bool s_rowDragged = false;

    bool RowSelected(Entity e)
    {
        return s_selectedRows.contains(e);
    }

    void HandleRowSelection(Entity e)
    {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        {
            s_pressedEntity = e;
            s_pressedMode = ImGui::GetIO().KeyCtrl ? EditorLayer::SelectMode::Toggle
                : ImGui::GetIO().KeyShift ? EditorLayer::SelectMode::Range
                : EditorLayer::SelectMode::Replace;
            s_rowDragged = false;
        }
        if (s_pressedEntity == e && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
            && ImGui::IsItemHovered() && !s_rowDragged)
        {
            s_clickEntity = e;
            s_clickMode = s_pressedMode;
            s_clickPending = true;
        }
    }

    void DrawEntityDragSource(Entity e, const std::string& name)
    {
        if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) return;
        s_rowDragged = true;
        if (RowSelected(e) && s_selection->size() > 1)
        {
            ImGui::SetDragDropPayload("ENTITY_SET", s_selection->data(), s_selection->size() * sizeof(Entity), ImGuiCond_Once);
            ImGui::Text("Move %zu selected entities to folder", s_selection->size());
        }
        else
        {
            ImGui::SetDragDropPayload("ENTITY", &e, sizeof(e), ImGuiCond_Once);
            ImGui::Text("Entity \"%s\"", name.c_str());
        }
        ImGui::EndDragDropSource();
    }

    void AcceptFolderEntityDrop(const std::string& path)
    {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_SET");
        if (!payload) payload = ImGui::AcceptDragDropPayload("ENTITY");
        if (!payload || payload->DataSize <= 0 || payload->DataSize % sizeof(Entity)) return;
        const auto* begin = static_cast<const Entity*>(payload->Data);
        s_pendingFolderSet.entities.assign(begin, begin + payload->DataSize / sizeof(Entity));
        s_pendingFolderSet.path = path;
        s_pendingFolderSet.valid = true;
    }

    void DrawEntityTree(World* world, Entity e,
                        Entity& selectedEntity,
                        Entity& hierarchyHighlight,
                        const HierarchyMap& hmap)
    {
        if (!world->IsAlive(e)) return;

        // Row order feeds Shift+click range selection: only the traversal knows
        // what is visually between two rows.
        s_rowOrder.push_back(e);

        std::uint32_t  kidCount = 0;
        const Entity*  kids     = hmap.ChildrenOf(e, kidCount);
        const bool hasChildren  = (kidCount != 0);
        const std::string& name = world->GetName(e);

        // Visual: cycle warning (orange) overrides the per-type icon + soft
        // tint. The icon is prefixed to the label so clicking still hits the
        // whole tree-node (keeps selection / drag-drop intact).
        const bool     isCyclic = hmap.IsCyclic(e);
        const HierIcon hicon    = CachedHierarchyIcon(world, e);
        const bool     pushCol  = isCyclic || hicon.tint;
        if (pushCol)
            ImGui::PushStyleColor(ImGuiCol_Text,
                isCyclic ? ImVec4(1.0f, 0.55f, 0.25f, 1.f) : hicon.color);

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow  |
            ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!hasChildren)  flags |= ImGuiTreeNodeFlags_Leaf;
        if (RowSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;

        ImGui::PushID(static_cast<int>(e));

        if (s_revealAncestors.count(e)) ImGui::SetNextItemOpen(true);
        bool open = false;
        if (s_renamingEntity == e)
        {
            // Collapsed/leaf tree node as placeholder, then overlay InputText.
            ImGuiTreeNodeFlags renameFlags = flags | ImGuiTreeNodeFlags_AllowOverlap;
            open = ImGui::TreeNodeEx("##node", renameFlags, "");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            auto commitRename = [&]() {
                if (s_renameBuffer[0] == '\0') return;
                // Capture the old name BEFORE SetName, queue for undo recording.
                s_pendingRename.entity  = e;
                s_pendingRename.oldName = world->GetName(e);
                s_pendingRename.newName = s_renameBuffer;
                world->SetName(e, s_renameBuffer);
            };
            if (s_renameWantsFocus)
            {
                ImGui::SetKeyboardFocusHere();
                s_renameWantsFocus = false;
            }
            if (ImGui::InputText("##rename", s_renameBuffer, sizeof(s_renameBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll))
            {
                commitRename();
                s_renamingEntity = NullEntity;
            }
            else if (!ImGui::IsItemActive() && ImGui::IsItemDeactivated())
            {
                commitRename();
                s_renamingEntity = NullEntity;
            }

            else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                s_renamingEntity = NullEntity;
            }
        }
        else
        {
            if (s_faIconsLoaded && hicon.icon)
                open = ImGui::TreeNodeEx("##node", flags, "%s  %s", hicon.icon, name.c_str());
            else
                open = ImGui::TreeNodeEx("##node", flags, "%s", name.c_str());
            if (e == s_revealEntity)
            {
                ImGui::SetScrollHereY(0.5f);
                s_revealEntity = NullEntity;
            }
            HandleRowSelection(e);
            DrawEntityDragSource(e, name);
            // Drag-drop target: accept onto this entity to make it the new
            // parent. Cycle check prevents dropping a parent into its own
            // descendant.
            if (ImGui::BeginDragDropTarget())
            {
                // A UI node dragged over the top / bottom quarter of another UI
                // node's row goes before / after it among that row's siblings (a
                // line shows where); the middle makes it a child, as for every row.
                int zone = 0;
                const ImGuiPayload* peek = ImGui::GetDragDropPayload();
                if (peek && peek->IsDataType("ENTITY") && peek->Data)
                {
                    const Entity d = *static_cast<const Entity*>(peek->Data);
                    if (d != e && world->HasComponent<UI::UIRect>(d) && world->HasComponent<UI::UIRect>(e))
                    {
                        const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                        const float  q  = (r1.y - r0.y) * 0.25f, my = ImGui::GetIO().MousePos.y;
                        zone = my < r0.y + q ? -1 : (my > r1.y - q ? 1 : 0);
                        if (zone != 0)
                        {
                            const float y = zone < 0 ? r0.y : r1.y;
                            ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x, y), ImVec2(r1.x, y),
                                                                IM_COL32(255, 200, 80, 255), 2.f);
                        }
                    }
                }
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("ENTITY", zone != 0 ? ImGuiDragDropFlags_AcceptNoDrawDefaultRect : 0))
                {
                    const Entity dragged = *static_cast<const Entity*>(payload->Data);
                    if (zone != 0)
                    {
                        s_pendingUIReorder.child  = dragged;
                        s_pendingUIReorder.anchor = e;
                        s_pendingUIReorder.after  = zone > 0;
                    }
                    else if (dragged != e && !WouldCreateCycle(world, dragged, e))
                    {
                        s_pendingReparent.child     = dragged;
                        s_pendingReparent.newParent = e;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Right-click preserves a multi-selection containing this row.
            if (ImGui::BeginPopupContextItem("##entity_ctx"))
            {
                if (!RowSelected(e) && ImGui::IsWindowAppearing())
                {
                    s_clickEntity = e;
                    s_clickMode = EditorLayer::SelectMode::Replace;
                    s_clickPending = true;
                }
                // A canvas, or a UI node (its canvas opens with the node selected).
                // Queued; UIEditorWindow::ProcessPendingActions opens it.
                if (world->HasComponent<UI::UICanvas>(e) || world->HasComponent<UI::UIRect>(e))
                {
                    if (ImGui::MenuItem("Edit in UI Editor")) Editor::UIEditorWindow::Get().RequestOpen(e);
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Rename", "F2"))
                {
                    s_renamingEntity   = e;
                    s_renameWantsFocus = true;
                    strncpy_s(s_renameBuffer, sizeof(s_renameBuffer), name.c_str(), _TRUNCATE);
                    s_renameBuffer[sizeof(s_renameBuffer) - 1] = '\0';
                }
                if (ImGui::MenuItem("Create Child"))
                    s_pendingCreateChild = e;
                if (ImGui::MenuItem("Wrap Visual as Child",
                                    nullptr, false,
                                    world->HasComponent<MeshHandle>(e)
                                 || world->HasComponent<MeshLibRef>(e)))
                {
                    s_pendingWrapVisual = e;
                }
                // A UI node unparents to its canvas (a UI node at the scene root
                // would never be laid out).
                if (world->HasComponent<UI::UIRect>(e))
                {
                    const Entity canvas = UI::FindOwningCanvas(*world, e);
                    if (ImGui::MenuItem("Unparent (to canvas)", nullptr, false,
                                        canvas != NullEntity && UI::ResolveUIParent(*world, e) != canvas))
                    {
                        s_pendingReparent.child     = e;
                        s_pendingReparent.newParent = NullEntity;
                    }
                }
                else if (ImGui::MenuItem("Unparent (drop to root)",
                                         nullptr, false,
                                         world->HasComponent<Parent>(e)))
                {
                    s_pendingReparent.child     = e;
                    s_pendingReparent.newParent = NullEntity;
                }
                // Follow a socket (UE's Attach To > socket). Refused whatever the
                // socket (a parent, inside an instance, playing): disabled, the
                // reason on hover.
                const std::string attachRefusal = s_attachRefusal ? s_attachRefusal(e) : std::string();
                if (!attachRefusal.empty())
                {
                    ImGui::MenuItem("Attach to Socket", nullptr, false, false);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", attachRefusal.c_str());
                }
                else if (ImGui::BeginMenu("Attach to Socket"))
                {
                    DrawAttachToSocketMenu(*world, e);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Remove from Folder", nullptr, false,
                                    world->HasComponent<EditorFolderComponent>(e)))
                {
                    s_pendingFolderSet.entity = e;
                    s_pendingFolderSet.path.clear();
                    s_pendingFolderSet.valid  = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate"))
                    s_pendingDuplicate = e;
                if (ImGui::MenuItem("Save as Prefab"))
                    s_pendingPrefabSave = e;

                // The model-wide material swap: push this instance's per-part
                // material assignments back into the model. Deliberately a separate,
                // explicit command rather than part of saving the scene: it rewrites
                // a shared source asset, so the new material becomes the default for
                // every scene and instance that spawns this .iscn. Saving the scene
                // persists the same swap per scene (the .iscene MR line) and touches
                // nothing outside it.
                {
                    const SceneSourcePath* ssp = world->GetComponent<SceneSourcePath>(e);
                    const bool isSceneRoot = (ssp && !ssp->path.empty());
                    if (ImGui::MenuItem("Write Materials to Source Model (.iscn)",
                                        nullptr, false, isSceneRoot))
                        s_pendingIscnMatWrite = e;
                    if (isSceneRoot && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Repoint '%s' at the materials this instance is "
                                          "using.\nAffects EVERY scene that spawns this model.",
                                          ssp->path.c_str());
                    // The instance becomes the scene's own nodes, each mesh a Static
                    // Mesh, so a part can be moved, hidden or deleted and the edit is
                    // saved.
                    if (ImGui::MenuItem("Unpack Model Instance", nullptr, false, isSceneRoot))
                        s_pendingUnpackModel = e;
                    if (isSceneRoot && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Turn this instance of '%s' into ordinary nodes whose\n"
                                          "meshes are Static Meshes. Edits to its parts are then\n"
                                          "saved with the scene. (Skinned models: not yet.)",
                                          ssp->path.c_str());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete"))
                    s_pendingDelete = e;
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();

        if (pushCol) ImGui::PopStyleColor();

        if (open)
        {
            for (std::uint32_t ki = 0; ki < kidCount; ++ki)
                DrawEntityTree(world, kids[ki], selectedEntity, hierarchyHighlight, hmap);
            ImGui::TreePop();
        }
    }

    // ---- Outliner folder tree (Unreal-style) ----
    // Rebuilt with the hierarchy view from each root entity's
    // EditorFolderComponent.path plus the editor's known-folders set (so empty
    // folders still show). Folders are pure editor organization: a child entity
    // nests under its real parent, so only roots are foldered.
    struct FolderNode
    {
        std::string                       fullPath;   // "" = root
        std::map<std::string, FolderNode> subs;       // leaf name -> child (sorted)
        std::vector<Entity>               ents;        // root entities directly here
    };

    // HierarchyViewCache (below) keeps the Hierarchy panel's derived view (logical
    // map + folder tree of roots) across frames. Building it is O(N) over every
    // entity, so it is rebuilt only when:
    //   * MarkHierarchyDirty() was called (editor-side structural edits),
    //   * the alive-entity count / world pointer / world epoch changed,
    //   * the Internal or Sections toggle flipped,
    //   * the safety net fires (catches script-side reparenting and component
    //     add / removes that change icons / ranks without a count change).
    // Entity names are deliberately not cached: DrawEntityTree reads
    // World::GetName live, so renames need no invalidation.

    // Safety-net period, ~2 s at 60 Hz. It only has to catch script-side
    // reparenting that changes no entity count (rare; two seconds of staleness is
    // not noticeable); a short period rebuilds visibly often at 15k entities.
    constexpr uint32_t kHierarchySafetyNetFrames = 120;

    struct HierarchyViewCache
    {
        HierarchyMap map;
        FolderNode   folderRoot;
        World*       world            = nullptr;
        uint32_t     epoch            = 0;
        size_t       entityCount      = 0;
        bool         showInternal     = false;
        bool         showSections     = false;
        uint32_t     framesSinceBuild = 0;
        bool         valid            = false;
    };
    HierarchyViewCache s_hierView;

    // Pin-order rank shared by the flat sort and the per-folder sort: scene
    // singletons (World Environment, Sky, ToD, GI, Camera) float to the top.
    int HierarchyEntityRank(World* w, Entity e)
    {
        if (w->HasComponent<WorldEnvironmentComponent>(e))          return 0;
        if (w->HasComponent<AtmosphereComponent>(e))                return 1;
        if (w->HasComponent<SkyboxComponent>(e))                    return 1;
        if (w->HasComponent<TODConfigComponent>(e))                 return 2;
        if (w->HasComponent<IndirectLightingSettingsComponent>(e))  return 3;
        if (w->HasComponent<CameraComponent>(e))                    return 4;
        return 1 << 30;
    }

    // Ensure the folder chain for `path` ("A/B/C") exists; return the leaf node.
    FolderNode* EnsureFolderChain(FolderNode& root, const std::string& path)
    {
        FolderNode* cur = &root;
        size_t start = 0;
        while (start < path.size())
        {
            const size_t slash = path.find('/', start);
            const size_t len   = (slash == std::string::npos) ? std::string::npos : slash - start;
            std::string  leaf  = path.substr(start, len);
            if (!leaf.empty())
            {
                FolderNode& child = cur->subs[leaf];
                if (child.fullPath.empty())
                    child.fullPath = cur->fullPath.empty() ? leaf : cur->fullPath + "/" + leaf;
                cur = &child;
            }
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return cur;
    }

    // rankById: HierarchyEntityRank precomputed per root (entity-id indexed); the
    // comparator must not probe components per comparison.
    void SortFolderRec(FolderNode& node, const std::vector<int>& rankById)
    {
        std::sort(node.ents.begin(), node.ents.end(), [&rankById](Entity a, Entity b) {
            const int ra = rankById[a], rb = rankById[b];
            if (ra != rb) return ra < rb;
            return a < b;
        });
        for (auto& [leaf, child] : node.subs) SortFolderRec(child, rankById);
    }

    // Recursively draw a folder node: sub-folders (collapsible) then entities.
    void DrawFolderNode(World* world, const FolderNode& node,
                        Entity& selectedEntity, Entity& hierarchyHighlight,
                        const HierarchyMap& hmap)
    {
        for (const auto& [leaf, child] : node.subs)
        {
            ImGui::PushID(child.fullPath.c_str());
            const ImVec4 folderCol(0.86f, 0.78f, 0.42f, 1.f);   // Unreal-ish gold
            ImGuiTreeNodeFlags ff = ImGuiTreeNodeFlags_SpanAvailWidth
                                  | ImGuiTreeNodeFlags_OpenOnArrow
                                  | ImGuiTreeNodeFlags_DefaultOpen;
            if (s_revealFolder == child.fullPath
                || s_revealFolder.starts_with(child.fullPath + "/"))
                ImGui::SetNextItemOpen(true);
            bool open;
            if (s_renamingFolder == child.fullPath)
            {
                open = ImGui::TreeNodeEx("##folder", ff | ImGuiTreeNodeFlags_AllowOverlap, "");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                auto commit = [&]() {
                    if (s_folderRenameBuffer[0] == '\0') return;
                    s_pendingFolderOp.kind = PendingFolderOp::Rename;
                    s_pendingFolderOp.path = child.fullPath;
                    s_pendingFolderOp.arg  = s_folderRenameBuffer;
                };
                if (ImGui::InputText("##frename", s_folderRenameBuffer, sizeof(s_folderRenameBuffer),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                { commit(); s_renamingFolder.clear(); }
                else if (!ImGui::IsItemActive() && ImGui::IsItemDeactivated())
                { commit(); s_renamingFolder.clear(); }
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                { s_renamingFolder.clear(); }
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, folderCol);
                if (s_faIconsLoaded)
                    open = ImGui::TreeNodeEx("##folder", ff, "%s  %s", FontAwesome::kFolder, leaf.c_str());
                else
                    open = ImGui::TreeNodeEx("##folder", ff, "[%s]", leaf.c_str());
                ImGui::PopStyleColor();

                // Drop target: entity -> set its folder; folder -> move under here.
                if (ImGui::BeginDragDropTarget())
                {
                    AcceptFolderEntityDrop(child.fullPath);
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FOLDER"))
                    {
                        s_pendingFolderOp.kind = PendingFolderOp::MoveFolder;
                        s_pendingFolderOp.path = static_cast<const char*>(pl->Data); // moved folder
                        s_pendingFolderOp.arg  = child.fullPath;                     // new parent
                    }
                    ImGui::EndDragDropTarget();
                }
                // Drag source: move this folder elsewhere.
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
                {
                    ImGui::SetDragDropPayload("FOLDER", child.fullPath.c_str(),
                                              child.fullPath.size() + 1);
                    ImGui::Text("Folder \"%s\"", leaf.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("##folderctx"))
                {
                    if (ImGui::MenuItem("New Subfolder"))
                    {
                        s_pendingFolderOp.kind = PendingFolderOp::Create;
                        s_pendingFolderOp.path = child.fullPath;
                        s_pendingFolderOp.arg  = "New Folder";
                    }
                    if (ImGui::MenuItem("Rename"))
                    {
                        s_renamingFolder = child.fullPath;
                        ::strncpy_s(s_folderRenameBuffer, leaf.c_str(), _TRUNCATE);
                    }
                    if (ImGui::MenuItem("Delete Folder"))
                    {
                        s_pendingFolderOp.kind = PendingFolderOp::Delete;
                        s_pendingFolderOp.path = child.fullPath;
                    }
                    ImGui::EndPopup();
                }
            }

            if (open)
            {
                DrawFolderNode(world, child, selectedEntity, hierarchyHighlight, hmap);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        // Entities that live directly in this folder.
        for (Entity e : node.ents)
            DrawEntityTree(world, e, selectedEntity, hierarchyHighlight, hmap);
    }
}

// Delete a set of subtree roots as ONE undo entry. Callers must pass roots
// (SelectionRoots()), never a raw selection: MakeDeleteAction destroys the
// whole subtree, so a separately-listed child would be destroyed twice.
void EditorLayer::DeleteEntities(const std::vector<Entity>& rootsIn)
{
    if (!m_world || rootsIn.empty()) return;
    // A UI Document's instance is its .iui's: read-only here, whichever key or
    // panel asked (the Outliner hides it, but a scene tab of the UI Editor
    // selects it).
    std::vector<Entity> roots;
    roots.reserve(rootsIn.size());
    for (const Entity e : rootsIn)
    {
        if (e != NullEntity && m_world->IsAlive(e) && UI::DocumentHostOf(*m_world, e) != NullEntity)
        {
            LOG_WARNING("EditorLayer: Delete skipped '%s' - a node of a UI Document instance is read-only (edit its "
                        ".iui, or delete its host)", m_world->GetName(e).c_str());
            continue;
        }
        roots.push_back(e);
    }
    if (roots.empty()) return;

    if (UndoEnabled())
    {
        // Snapshots are captured against the LIVE world here; the macro
        // performs every delete when Execute runs, so no snapshot is ever taken
        // of an already-destroyed subtree.
        SyncedEditContext();
        // EditTarget: a UI asset tab's delete is a step of its own ring.
        Editor::Undo::EditHistory& h = EditTarget();
        h.BeginMacro(roots.size() == 1
            ? std::string("Delete ") + m_world->GetName(roots[0])
            : std::string("Delete ") + std::to_string(roots.size()) + " entities");
        for (Entity e : roots)
        {
            if (e == NullEntity || !m_world->IsAlive(e)) continue;
            Editor::Undo::EntitySubtreeSnapshot snap =
                Editor::Undo::EntitySubtreeSnapshot::Capture(*m_world, e);
            h.Execute(Editor::Undo::MakeDeleteAction(
                std::move(snap), m_world->GetName(e)));
        }
        h.EndMacro();
    }
    else
    {
        for (Entity e : roots)
            if (e != NullEntity && m_world->IsAlive(e)) DestroyEntityRecursive(m_world, e);
    }

    // A UI asset tab's node is not the scene's selection; the tab is told instead.
    if (m_editSink)
    {
        if (m_editSink->changed) m_editSink->changed();
    }
    else
        ClearEntitySelection();
    MarkHierarchyDirty();
}

// Editor.Reparent and the Outliner drag share these refusals, ReparentEntity
// and the undo record.
bool EditorLayer::ReparentWithUndo(Entity child, Entity newParent, std::string* error)
{
    auto fail = [error](std::string why) { if (error) *error = std::move(why); return false; };
    if (!m_world || child == NullEntity || !m_world->IsAlive(child))
        return fail("stale_handle: the entity is not alive");
    if (newParent != NullEntity && !m_world->IsAlive(newParent))
        return fail("stale_handle: the new parent is not alive");
    // A Canvas UI node moves on its UIParent edge (NullEntity = under its canvas),
    // last among the new siblings.
    if (m_world->HasComponent<UI::UIRect>(child))
        return MoveUINode(child, newParent, -1, error);
    if (newParent != NullEntity && UI::IsUINode(*m_world, newParent))
        return fail(m_world->HasComponent<UI::UICanvas>(child)
                        ? "bad_value: a canvas is a root - it has no UI parent"
                        : "bad_value: only a UI node goes under a canvas or a UI node");
    if (newParent == child || (newParent != NullEntity && WouldCreateCycle(m_world, child, newParent)))
        return fail("cycle: the new parent is the entity or one of its descendants");

    Entity         oldParent   = NullEntity;
    LocalTransform oldLocal{};
    bool           hasOldLocal = false;
    if (const Parent* p = m_world->GetComponent<Parent>(child)) oldParent = p->entity;
    if (const LocalTransform* lt = m_world->GetComponent<LocalTransform>(child)) { oldLocal = *lt; hasOldLocal = true; }
    if (oldParent == newParent) return true;   // nothing to do, nothing recorded

    ReparentEntity(m_world, child, newParent);
    MarkHierarchyDirty();

    if (UndoEnabled())
    {
        SyncedEditContext();
        LocalTransform newLocal{};
        bool           hasNewLocal = false;
        if (const LocalTransform* lt = m_world->GetComponent<LocalTransform>(child)) { newLocal = *lt; hasNewLocal = true; }
        PushEdit(std::make_unique<Editor::Undo::ReparentAction>(
            m_world->MakeHandle(child), oldParent, oldLocal, hasOldLocal,
            newParent, newLocal, hasNewLocal, m_world->GetName(child)));
    }
    return true;
}

// Refused: a child that is no UIRect node (a canvas root is no one's child), a
// parent that is no UI node, a cycle. Nothing is recorded when nothing would
// change.
bool EditorLayer::MoveUINode(Entity child, Entity newParent, int index, std::string* error)
{
    auto fail = [error](std::string why) { if (error) *error = std::move(why); return false; };
    if (!m_world || child == NullEntity || !m_world->IsAlive(child))
        return fail("stale_handle: the entity is not alive");
    World& w = *m_world;
    if (!w.HasComponent<UI::UIRect>(child))
        return fail(w.HasComponent<UI::UICanvas>(child)
                        ? "bad_value: a canvas is a root - it has no UI parent"
                        : "bad_value: not a UI node (no UIRect)");
    // A UI Document's instance is the .iui's (made at run time, replaced when the
    // file changes): nothing moves in or out of it here.
    if (UI::DocumentHostOf(w, child) != NullEntity ||
        (newParent != NullEntity && UI::DocumentHostOf(w, newParent) != NullEntity))
        return fail("refused: a node of a UI Document instance is read-only - edit its .iui");
    if (newParent == NullEntity)
    {
        newParent = UI::FindOwningCanvas(w, child);
        if (newParent == NullEntity) return fail("bad_value: the node belongs to no canvas to move it under");
    }
    if (!w.IsAlive(newParent)) return fail("stale_handle: the new parent is not alive");
    if (!UI::IsUINode(w, newParent))
        return fail("bad_value: a UI node's parent is a canvas or another UI node, not '" + w.GetName(newParent) + "'");
    if (newParent == child || UI::UIWouldCycle(w, child, newParent))
        return fail("cycle: the new parent is the node or one of its descendants");
    // A UI Editor stage (an asset / template tab's tree) and the scene, or two
    // stages, never trade nodes: each has its own undo history and its own file,
    // and a node carried across would be recorded in the wrong one (and saved by
    // neither, or by the wrong one). With a stage on either side, the move stays
    // in its tree.
    {
        auto stage = [&w](Entity x)
        {
            const PreviewStageTag* st = w.GetComponent<PreviewStageTag>(x);
            return st && st->owner == PreviewStageTag::kUIEditor;
        };
        auto treeRoot = [&w](Entity x)
        {
            for (int guard = 0; guard < 4096; ++guard)
            {
                const Entity p = UI::ResolveUIParent(w, x);
                if (p == NullEntity || p == x) break;
                x = p;
            }
            return x;
        };
        if ((stage(child) || stage(newParent)) && treeRoot(child) != treeRoot(newParent))
            return fail("refused: a UI Editor tab's stage and the scene (or another tab) never trade nodes - a node "
                        "moves within its own tree");
    }

    // The new sibling order: the parent's children in paint order, the child
    // taken out and put back at `index`.
    std::vector<Entity> order;
    UI::UIChildrenOf(w, newParent, order);
    order.erase(std::remove(order.begin(), order.end(), child), order.end());
    const int n = static_cast<int>(order.size());
    const int at = (index < 0 || index > n) ? n : index;
    order.insert(order.begin() + at, child);

    const Entity oldParent = UI::ResolveUIParent(w, child);
    bool changed = oldParent != newParent;
    for (int i = 0; i < static_cast<int>(order.size()) && !changed; ++i)
        if (const UI::UIRect* r = w.GetComponent<UI::UIRect>(order[i]); r && r->siblingIndex != i) changed = true;
    if (!changed) return true;

    std::vector<UIMoveAction::NodeState> before, after;
    for (Entity e : order) before.push_back(UIMoveAction::Capture(w, e));
    const bool hadGuid = w.HasComponent<GuidComponent>(newParent);
    if (oldParent != newParent) UI::AttachUIParent(w, child, newParent);
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
        if (UI::UIRect* r = w.GetComponent<UI::UIRect>(order[i])) r->siblingIndex = i;
    for (Entity e : order) after.push_back(UIMoveAction::Capture(w, e));

    MarkHierarchyDirty();
    if (UndoEnabled())
    {
        SyncedEditContext();
        const std::string label = oldParent != newParent ? "Reparent UI " + w.GetName(child)
                                                         : "Reorder UI " + w.GetName(child);
        // The GUID the attach stamped on a parent that had none goes back with
        // the move: one step, the scene exactly as before.
        EditTarget().BeginMacro(label);
        PushEdit(std::make_unique<UIMoveAction>(std::move(before), std::move(after), label));
        if (!hadGuid)
            if (auto stamp = Editor::UITemplates::GuidStampAction(w, newParent)) PushEdit(std::move(stamp));
        EditTarget().EndMacro();
    }
    LOG_INFO("EditorLayer: UI node %u ('%s') -> parent %u ('%s') at %d of %zu", child, w.GetName(child).c_str(),
             newParent, w.GetName(newParent).c_str(), at, order.size());
    return true;
}

std::vector<Entity> EditorLayer::OutlinerChildrenOf(Entity e)
{
    std::vector<Entity> out;
    if (!m_world) return out;
    HierarchyMap map;
    BuildHierarchyMap(m_world, map, !m_showMeshSections);
    std::uint32_t n = 0;
    if (const Entity* kids = map.ChildrenOf(e, n)) out.assign(kids, kids + n);
    return out;
}

bool EditorLayer::AuthoredCanvas(const World& world, Entity canvas)
{
    if (canvas == NullEntity || !world.IsAlive(canvas) || !world.HasComponent<UI::UICanvas>(canvas)) return false;
    return !world.HasComponent<SceneTransientTag>(canvas) && !world.HasComponent<PreviewStageTag>(canvas) &&
           !world.HasComponent<UI::UICanvasOffscreen>(canvas) && !world.HasComponent<UI::UIDocumentMember>(canvas);
}

// Detached (no Prefab Ref): a linked instance saves as a bare reference, which
// would drop its root's UIParent and every UI edit made in the scene.
bool EditorLayer::AdoptUIPrefabRoot(Entity root, Entity target, std::string* why)
{
    if (!m_world || !UI::IsUINode(*m_world, root)) return false;
    World& w = *m_world;
    if (w.HasComponent<PrefabSourcePath>(root)) w.RemoveComponent<PrefabSourcePath>(root);
    MarkHierarchyDirty();
    if (w.HasComponent<UI::UICanvas>(root)) return true;   // a canvas is a root of its own

    Entity parent = target;
    if (parent == NullEntity)
    {
        // Only a canvas the scene authors: a stage's or an instance's would take
        // the prefab out of the scene file, or lose it at the next respawn.
        parent = UI::FindOwningCanvas(w, m_selectedEntity);
        if (parent != NullEntity && !AuthoredCanvas(w, parent)) parent = NullEntity;
        if (parent == NullEntity)
            if (const auto* cp = w.GetPool<UI::UICanvas>())
                for (const Entity c : cp->Entities())
                    if (c != root && AuthoredCanvas(w, c)) { parent = c; break; }
    }
    if (parent == NullEntity || !UI::IsUINode(w, parent) || UI::UIWouldCycle(w, root, parent))
    {
        const std::string reason = parent == NullEntity
            ? std::string("no canvas to put the UI prefab under - create a Canvas first")
            : "'" + w.GetName(parent) + "' is not a canvas or a UI node it can go under";
        LOG_WARNING("EditorLayer: UI prefab root %u ('%s') is not drawn: %s", root, w.GetName(root).c_str(),
                    reason.c_str());
        if (why) *why = reason;
        return true;
    }
    std::vector<Entity> siblings;
    UI::UIChildrenOf(w, parent, siblings);
    int next = 0;
    for (const Entity s : siblings)
        if (s != root)
            if (const UI::UIRect* r = w.GetComponent<UI::UIRect>(s)) next = (std::max)(next, r->siblingIndex + 1);
    UI::AttachUIParent(w, root, parent);
    if (UI::UIRect* r = w.GetComponent<UI::UIRect>(root)) r->siblingIndex = next;
    LOG_INFO("EditorLayer: UI prefab root %u ('%s') placed under %u ('%s'), detached from its .ipfb", root,
             w.GetName(root).c_str(), parent, w.GetName(parent).c_str());
    return true;
}

// Duplicate every selection root, then select the copies, so the obvious next
// gesture (drag the duplicates aside) needs no re-pick.
void EditorLayer::DuplicateSelection()
{
    if (!m_world) return;
    const std::vector<Entity> roots = SelectionRoots();
    if (roots.empty()) return;

    m_editHistory.BeginMacro(std::string("Duplicate ")
                             + std::to_string(roots.size()) + " entities");
    std::vector<Entity> copies;
    copies.reserve(roots.size());
    for (Entity e : roots)
    {
        if (e == NullEntity || !m_world->IsAlive(e)) continue;
        const Entity copy = DuplicateEntity(e);
        if (copy != NullEntity) copies.push_back(copy);
    }
    m_editHistory.EndMacro();

    if (copies.empty()) return;
    ClearEntitySelection();
    for (Entity c : copies) SelectEntity(c, SelectMode::Add);
    m_hierarchyHighlight     = m_selectedEntity;
    m_lastSeenSelectedEntity = m_selectedEntity;
    MarkHierarchyDirty();
}

void EditorLayer::RenderHierarchyPanel()
{
    // Edit > Rename / F2 is answered BY this panel, so it reopens a closed one.
    if (m_hierarchyRenameRequest != NullEntity) m_showHierarchy = true;
    // Closed from its tab's X (Window > General reopens it). Pending reveals
    // stay queued and are honoured when it comes back.
    if (!m_showHierarchy) return;

    if (m_hierarchyRevealEntity != NullEntity)
    {
        ImGui::SetNextWindowCollapsed(false);
        ImGui::SetNextWindowFocus();
    }
    // Early-out when the tab is hidden / collapsed: the body is O(N) over all
    // entities and nothing in it is load-bearing for other panels (pending ops are
    // set and applied within one visible frame).
    if (!ImGui::Begin(kWindowHierarchy, &m_showHierarchy))
    {
        ImGui::End();
        return;
    }

    if (!m_world)
    {
        ImGui::TextDisabled("(no active scene)");
        ImGui::End();
        return;
    }
    // The context menus' Attach to Socket asks this (only while a menu is open).
    s_attachRefusal = [this](Entity e) { return AttachRefusal(e); };

    // ---- "+ Create": the main menu's Create list (Unity's Hierarchy "+") ----
    // Same items, same undo recording (RecordSpawn); Empty Entity is the first
    // entry.
    if (ImGui::Button("+ Create"))
        ImGui::OpenPopup("##hierCreate");
    if (ImGui::BeginPopup("##hierCreate"))
    {
        DrawCreateMenuItems();
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("+ Folder"))
    {
        // Create a root-level "New Folder" (de-duplicated), ready to rename/fill.
        std::string base = "New Folder", cand = base;
        for (int n = 1; m_editorFolders.count(cand); ++n) cand = base + " " + std::to_string(n + 1);
        m_editorFolders.insert(cand);
        MarkHierarchyDirty();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // Editor-internal entities (editor camera, camera-stack channel, the Chain
    // Physics Preview stage) are hidden by default; this toggle reveals them.
    ImGui::Checkbox("Internal", &m_showInternalEntities);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show editor-internal entities (Editor Camera, camera-stack channel)");
    ImGui::SameLine();
    ImGui::Checkbox("Sections", &m_showMeshSections);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the section entities a Static Mesh derives (one per material slot).\n"
                          "They are rebuilt from the Static Mesh and never saved: edit the owner.");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Right-click a row for options; drag onto another row to parent,\n"
                          "onto a folder to file it, onto empty space to unparent.\n"
                          "Del delete  ·  F2 rename  ·  Ctrl+D duplicate  ·  Ctrl+A select all\n"
                          "F focus in viewport  ·  Esc deselect");

    // Edit > Rename / F2: open the inline rename field on that row. The row
    // state is TU-local, so the request crosses over through a member.
    if (m_hierarchyRenameRequest != NullEntity)
    {
        const Entity e = m_hierarchyRenameRequest;
        m_hierarchyRenameRequest = NullEntity;
        if (m_world->IsAlive(e))
        {
            s_renamingEntity   = e;
            s_renameWantsFocus = true;
            strncpy_s(s_renameBuffer, sizeof(s_renameBuffer), m_world->GetName(e).c_str(), _TRUNCATE);
            s_renameBuffer[sizeof(s_renameBuffer) - 1] = '\0';
            m_hierarchyRevealEntity = e;   // expand + scroll to it
        }
    }

    // ---- Name filter: flat results while active ----
    // The tree hides a match deep inside a collapsed folder, so an active filter
    // swaps the folder tree for a flat match list.
    static ImGuiTextFilter s_hierFilter;
    if (m_hierarchyRevealEntity != NullEntity) s_hierFilter.Clear();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##hierFilter", "Filter entities by name...",
                                 s_hierFilter.InputBuf,
                                 IM_ARRAYSIZE(s_hierFilter.InputBuf)))
        s_hierFilter.Build();
    ImGui::Separator();

    // Reset the deferred ops: the walk below sets them, the code after it applies
    // them.
    s_pendingDelete      = NullEntity;
    s_pendingCreateChild = NullEntity;
    s_pendingDuplicate   = NullEntity;
    s_pendingPrefabSave  = NullEntity;
    s_pendingWrapVisual  = NullEntity;
    s_pendingIscnMatWrite = NullEntity;
    s_pendingUnpackModel = NullEntity;
    s_pendingReparent    = {};
    s_pendingUIReorder   = {};
    s_pendingAttach      = {};
    s_pendingRename      = {};
    s_pendingFolderSet   = {};
    s_pendingFolderOp    = {};

    // Logical hierarchy map (UIParent + Parent + FollowSocket + FollowEntity,
    // cycle detection) + folder view, cached across frames: see
    // HierarchyViewCache for the invalidation contract.
    ++s_hierView.framesSinceBuild;
    const size_t aliveCount = m_world->GetEntities().size();
    if (m_hierarchyStructureDirty
        || !s_hierView.valid
        || s_hierView.world        != m_world
        || s_hierView.epoch        != m_world->Epoch()
        || s_hierView.entityCount  != aliveCount
        || s_hierView.showInternal != m_showInternalEntities
        || s_hierView.showSections != m_showMeshSections
        || s_hierView.framesSinceBuild >= kHierarchySafetyNetFrames)
    {
        BuildHierarchyMap(m_world, s_hierView.map, !m_showMeshSections);
        const auto* sectionPool = m_showMeshSections ? nullptr : m_world->GetPool<MeshSectionOf>();

        // Roots = entities with no logical parent. Children draw recursively
        // inside DrawEntityTree; ROOT entities are additionally bucketed into
        // editor folders (Unreal-style) by their EditorFolderComponent.path.
        const auto* editorCamPool = m_world->GetPool<EditorCameraTag>();
        const auto* camStackPool  = m_world->GetPool<CameraStackChannelSingleton>();
        // The Chain Physics Preview panel's hidden stage is editor plumbing
        // too: a user who finds a character at y=-4096 in the tree would
        // select, save or delete it out from under the panel. Same
        // Internal-toggle exception as the two above.
        const auto* chainStagePool = m_world->GetPool<PreviewStageTag>();
        const auto* folderPool    = m_world->GetPool<EditorFolderComponent>();
        const auto* docMemberPool = m_world->GetPool<UI::UIDocumentMember>();   // a UI Document's instance: not rows
        std::vector<Entity> roots;
        for (Entity e : m_world->GetEntities())
        {
            if (!m_world->IsAlive(e)) continue;
            if (docMemberPool && docMemberPool->Has(e)) continue;
            // Editor plumbing, not scene content: the editor free camera, the
            // camera-stack channel entity and the preview stage. Hidden unless the
            // Internal toggle is on.
            if (!m_showInternalEntities
                && ((editorCamPool && editorCamPool->Has(e))
                    || (camStackPool && camStackPool->Has(e))
                    || (chainStagePool && chainStagePool->Has(e))))
                continue;
            if (sectionPool && sectionPool->Has(e)) continue;   // a hidden StaticMesh section
            const Entity p = s_hierView.map.ParentOf(e);
            if (p == NullEntity) roots.push_back(e);
        }

        // Folder tree: known folders (incl. empty user-created ones) + every root
        // entity's own path, then distribute roots into their folder. Entities in
        // no folder land at the root; within each folder they sort by pin-rank
        // (scene singletons first) then id.
        s_hierView.folderRoot = {};
        for (Entity e : roots)
            if (const auto* f = folderPool ? folderPool->Get(e) : nullptr; f && !f->path.empty())
                m_editorFolders.insert(f->path);
        for (const std::string& fp : m_editorFolders)
            EnsureFolderChain(s_hierView.folderRoot, fp);
        for (Entity e : roots)
        {
            const auto* f = folderPool ? folderPool->Get(e) : nullptr;
            FolderNode* node = (f && !f->path.empty())
                ? EnsureFolderChain(s_hierView.folderRoot, f->path)
                : &s_hierView.folderRoot;
            node->ents.push_back(e);
        }
        std::vector<int> rankById(m_world->EntityIdBound(), 1 << 30);
        for (Entity e : roots) rankById[e] = HierarchyEntityRank(m_world, e);
        SortFolderRec(s_hierView.folderRoot, rankById);

        s_hierIconCache.clear();
        s_hierView.world            = m_world;
        s_hierView.epoch            = m_world->Epoch();
        s_hierView.entityCount      = aliveCount;
        s_hierView.showInternal     = m_showInternalEntities;
        s_hierView.showSections     = m_showMeshSections;
        s_hierView.framesSinceBuild = 0;
        s_hierView.valid            = true;
        m_hierarchyStructureDirty   = false;
    }
    const HierarchyMap& hmap = s_hierView.map;
    s_revealEntity = m_hierarchyRevealEntity;
    s_revealAncestors.clear();
    s_revealFolder.clear();
    if (s_revealEntity != NullEntity && m_world->IsAlive(s_revealEntity))
    {
        Entity root = s_revealEntity;
        for (int depth = 0; depth < 512; ++depth)
        {
            const Entity parent = hmap.ParentOf(root);
            if (parent == NullEntity || !s_revealAncestors.insert(parent).second) break;
            root = parent;
        }
        if (const auto* folder = m_world->GetComponent<EditorFolderComponent>(root))
            s_revealFolder = folder->path;
    }
    m_hierarchyRevealEntity = NullEntity;

    // Hand the traversal the current selection (so rows can paint themselves)
    // and a fresh row-order buffer (so Shift+click has a flattened order to
    // range over). Both are collected back below the traversal.
    s_selection    = &m_selectedEntities;
    s_selectedRows.clear();
    s_selectedRows.insert(m_selectedEntities.begin(), m_selectedEntities.end());
    s_clickPending = false;
    s_rowOrder.clear();

    if (s_hierFilter.IsActive())
    {
        // Flat list. Parent name rides along dimmed for disambiguation;
        // Click and drag use the same selection semantics as tree rows.
        const auto* editorCamPool = m_world->GetPool<EditorCameraTag>();
        const auto* camStackPool  = m_world->GetPool<CameraStackChannelSingleton>();
        const auto* chainStagePool = m_world->GetPool<PreviewStageTag>();   // the Chain Physics Preview stage
        const auto* hiddenSections = m_showMeshSections ? nullptr : m_world->GetPool<MeshSectionOf>();
        const auto* docMemberPool  = m_world->GetPool<UI::UIDocumentMember>();   // a UI Document's instance: not rows
        int shown = 0;
        for (Entity e : m_world->GetEntities())
        {
            if (!m_world->IsAlive(e)) continue;
            if (hiddenSections && hiddenSections->Has(e)) continue;
            if (docMemberPool && docMemberPool->Has(e)) continue;
            if (!m_showInternalEntities
                && ((editorCamPool && editorCamPool->Has(e))
                    || (camStackPool && camStackPool->Has(e))
                    || (chainStagePool && chainStagePool->Has(e))))
                continue;
            const std::string& name = m_world->GetName(e);
            if (!s_hierFilter.PassFilter(name.c_str())) continue;
            ++shown;
            s_rowOrder.push_back(e);
            ImGui::PushID(static_cast<int>(e));
            ImGui::Selectable(name.empty() ? "(unnamed)" : name.c_str(), RowSelected(e),
                              ImGuiSelectableFlags_AllowDoubleClick);
            HandleRowSelection(e);
            DrawEntityDragSource(e, name);
            if (const Entity par = hmap.ParentOf(e); par != NullEntity)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("< %s", m_world->GetName(par).c_str());
            }
            ImGui::PopID();
        }
        if (shown == 0) ImGui::TextDisabled("(no entity matches '%s')", s_hierFilter.InputBuf);
    }
    else
    {
        DrawFolderNode(m_world, s_hierView.folderRoot, m_selectedEntity, m_hierarchyHighlight, hmap);
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        s_pressedEntity = NullEntity;
        s_rowDragged = false;
    }

    // Apply the click AFTER the traversal: SelectEntity moves the primary and
    // the row order, and doing that mid-traversal would change what the rows
    // below the clicked one test themselves against.
    m_hierarchyRowOrder.swap(s_rowOrder);
    s_selection = nullptr;
    if (s_clickPending)
    {
        SelectEntity(s_clickEntity, s_clickMode);
        m_hierarchyHighlight = m_selectedEntity;
        // The reconcile at the top of the next frame compares m_selectedEntity
        // against the vector's back(); they already agree, so it will not
        // collapse the multi-selection we just built.
        m_lastSeenSelectedEntity = m_selectedEntity;
        s_clickPending = false;
    }

    // Ctrl+A selects the visible/filtered entity rows, not collapsed content.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::IsAnyItemActive() && ImGui::GetIO().KeyCtrl
        && ImGui::IsKeyPressed(ImGuiKey_A, false))
    {
        m_selectedEntities = m_hierarchyRowOrder;
        m_selectedEntity = m_selectedEntities.empty() ? NullEntity : m_selectedEntities.back();
        m_selectionRangeAnchor = m_selectedEntity;
        m_hierarchyHighlight = m_selectedEntity;
        m_lastSeenSelectedEntity = m_selectedEntity;
    }

    // ---- Root-level drop zone ----
    // Dragging an entity here detaches it from its parent and makes it a
    // top-level entity; sized to fill the remaining space, so a drop anywhere
    // below the last root works.
    {
        const float remaining = std::max(40.0f, ImGui::GetContentRegionAvail().y);
        ImGui::InvisibleButton("##root_drop_zone", ImVec2(-1.f, remaining));
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("ENTITY"))
            {
                const Entity dragged = *static_cast<const Entity*>(payload->Data);
                s_pendingReparent.child     = dragged;
                s_pendingReparent.newParent = NullEntity;
                // Dropping onto empty space also clears the outliner folder.
                s_pendingFolderSet.entity = dragged;
                s_pendingFolderSet.direct = true;
                s_pendingFolderSet.path.clear();
                s_pendingFolderSet.valid  = true;
            }
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ENTITY_SET"))
            {
                if (pl->DataSize > 0 && pl->DataSize % sizeof(Entity) == 0)
                {
                    const auto* begin = static_cast<const Entity*>(pl->Data);
                    s_pendingFolderSet.entities.assign(begin, begin + pl->DataSize / sizeof(Entity));
                    s_pendingFolderSet.path.clear();
                    s_pendingFolderSet.valid = true;
                }
            }
            if (const ImGuiPayload* fpl = ImGui::AcceptDragDropPayload("FOLDER"))
            {
                s_pendingFolderOp.kind = PendingFolderOp::MoveFolder;
                s_pendingFolderOp.path = static_cast<const char*>(fpl->Data);
                s_pendingFolderOp.arg.clear();   // move to root
            }
            ImGui::EndDragDropTarget();
        }
        // Light visual hint while a drag is active over the zone.
        if (ImGui::IsItemHovered() && ImGui::GetDragDropPayload() &&
            (ImGui::GetDragDropPayload()->IsDataType("ENTITY") ||
             ImGui::GetDragDropPayload()->IsDataType("ENTITY_SET") ||
             ImGui::GetDragDropPayload()->IsDataType("FOLDER")))
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            dl->AddRect(mn, mx, IM_COL32(120, 200, 255, 200), 2.0f, 0, 1.5f);
        }
    }

    // A folder groups logical hierarchy roots; moving a child files its owning
    // tree rather than detaching prefab content. De-duplicate shared roots.
    if (s_pendingFolderSet.valid)
    {
        auto targets = s_pendingFolderSet.entities;
        if (targets.empty() && s_pendingFolderSet.entity != NullEntity)
        {
            if (IsEntitySelected(s_pendingFolderSet.entity)) targets = m_selectedEntities;
            else targets.push_back(s_pendingFolderSet.entity);
        }
        MoveEntitiesToFolder(targets, s_pendingFolderSet.path, !s_pendingFolderSet.direct);
    }
    s_pendingFolderSet = {};

    if (s_pendingFolderOp.kind != PendingFolderOp::None)
    {
        auto sanitize = [](std::string s){ for (char& c : s) if (c == '/') c = '_'; return s; };
        // Rewrite every entity path + folder-set entry: prefix oldP -> newP.
        auto rewritePrefix = [&](const std::string& oldP, const std::string& newP)
        {
            for (Entity e : m_world->GetEntities())
            {
                if (!m_world->IsAlive(e)) continue;
                auto* f = m_world->GetComponent<EditorFolderComponent>(e);
                if (!f) continue;
                const std::string P = f->path;
                if (P == oldP)
                {
                    if (newP.empty()) m_world->RemoveComponent<EditorFolderComponent>(e);
                    else              f->path = newP;
                }
                else if (P.size() > oldP.size() && P.compare(0, oldP.size(), oldP) == 0
                         && P[oldP.size()] == '/')
                {
                    const std::string suffix = P.substr(oldP.size());   // "/x/y"
                    f->path = newP.empty() ? suffix.substr(1) : newP + suffix;
                }
            }
            std::set<std::string> upd;
            for (const std::string& fp : m_editorFolders)
            {
                if (fp == oldP) { if (!newP.empty()) upd.insert(newP); }
                else if (fp.size() > oldP.size() && fp.compare(0, oldP.size(), oldP) == 0
                         && fp[oldP.size()] == '/')
                    upd.insert(newP.empty() ? fp.substr(oldP.size() + 1) : newP + fp.substr(oldP.size()));
                else upd.insert(fp);
            }
            m_editorFolders.swap(upd);
        };

        auto& op = s_pendingFolderOp;
        if (op.kind == PendingFolderOp::Create)
        {
            const std::string leaf = sanitize(op.arg.empty() ? "New Folder" : op.arg);
            const std::string full = op.path.empty() ? leaf : op.path + "/" + leaf;
            std::string cand = full;
            for (int n = 1; m_editorFolders.count(cand); ++n) cand = full + " " + std::to_string(n + 1);
            m_editorFolders.insert(cand);
        }
        else if (op.kind == PendingFolderOp::Rename)
        {
            const std::string leaf = sanitize(op.arg);
            if (!leaf.empty() && !op.path.empty())
            {
                const size_t slash    = op.path.find_last_of('/');
                const std::string par = (slash == std::string::npos) ? "" : op.path.substr(0, slash);
                const std::string nf  = par.empty() ? leaf : par + "/" + leaf;
                if (nf != op.path) rewritePrefix(op.path, nf);
            }
        }
        else if (op.kind == PendingFolderOp::Delete)
        {
            const size_t slash    = op.path.find_last_of('/');
            const std::string par = (slash == std::string::npos) ? "" : op.path.substr(0, slash);
            rewritePrefix(op.path, par);   // move contents up to the parent folder
        }
        else if (op.kind == PendingFolderOp::MoveFolder)
        {
            const std::string src = op.path, newParent = op.arg;
            const bool intoSelf = (newParent == src)
                || (newParent.size() > src.size() && newParent.compare(0, src.size(), src) == 0
                    && newParent[src.size()] == '/');
            if (!intoSelf && !src.empty())
            {
                const size_t slash    = src.find_last_of('/');
                const std::string leaf = (slash == std::string::npos) ? src : src.substr(slash + 1);
                const std::string nf   = newParent.empty() ? leaf : newParent + "/" + leaf;
                if (nf != src) rewritePrefix(src, nf);
            }
        }
        MarkHierarchyDirty();
    }
    s_pendingFolderOp = {};

    // ---- Process pending operations (after the tree draw, to avoid iterator issues) ----
    // Rename: the SetName already applied in DrawEntityTree; record the undo here.
    if (s_pendingRename.entity != NullEntity)
    {
        if (UndoEnabled() && m_world && m_world->IsAlive(s_pendingRename.entity)
            && s_pendingRename.oldName != s_pendingRename.newName)
        {
            SyncedEditContext();
            PushEdit(std::make_unique<Editor::Undo::RenameAction>(
                m_world->MakeHandle(s_pendingRename.entity),
                s_pendingRename.oldName, s_pendingRename.newName));
        }
        s_pendingRename = {};
    }

    if (s_pendingCreateChild != NullEntity)
    {
        Entity parent = s_pendingCreateChild;
        Entity child  = m_world->CreateEntity();
        if (UI::IsUINode(*m_world, parent))
        {
            // Under a canvas or a UI node, a UI node (a UIRect on the UIParent
            // edge, last among its siblings): a transform child there would be
            // neither laid out nor drawn.
            m_world->SetName(child, "UI Node");
            std::vector<Entity> siblings;
            UI::UIChildrenOf(*m_world, parent, siblings);
            UI::UIRect r;
            for (Entity s : siblings)
                if (const UI::UIRect* sr = m_world->GetComponent<UI::UIRect>(s))
                    r.siblingIndex = (std::max)(r.siblingIndex, sr->siblingIndex + 1);
            m_world->AddComponent<UI::UIRect>(child, r);
            UI::AttachUIParent(*m_world, child, parent);
        }
        else
        {
            m_world->SetName(child, "Entity");
            m_world->AddComponent<LocalTransform>(child, LocalTransform{});
            m_world->AddComponent<GlobalTransform>(child, GlobalTransform{});

            Parent pc;  pc.entity = parent;
            m_world->AddComponent<Parent>(child, pc);

            Children* ch = m_world->GetComponent<Children>(parent);
            if (ch)
                ch->entities.push_back(child);
            else
            {
                Children newCh;  newCh.entities.push_back(child);
                m_world->AddComponent<Children>(parent, std::move(newCh));
            }
        }
        m_selectedEntity     = child;

        if (UndoEnabled())
        {
            Editor::Undo::EntitySubtreeSnapshot snap =
                Editor::Undo::EntitySubtreeSnapshot::Capture(*m_world, child);
            SyncedEditContext();
            PushEdit(Editor::Undo::MakeCreateAction(std::move(snap), "Create Entity"));
        }
        s_pendingCreateChild = NullEntity;
        MarkHierarchyDirty();
    }

    if (s_pendingDelete != NullEntity)
    {
        // Deleting a row that belongs to a multi-selection deletes the whole
        // selection. ROOTS only: a subtree delete already carries its children,
        // and deleting a child separately would destroy an id twice.
        std::vector<Entity> targets;
        if (m_selectedEntities.size() > 1 && IsEntitySelected(s_pendingDelete))
            targets = SelectionRoots();
        else
            targets.push_back(s_pendingDelete);

        DeleteEntities(targets);
        s_pendingDelete = NullEntity;
    }

    if (s_pendingDuplicate != NullEntity)
    {
        // Sibling duplicate (cloned entity inherits source's parent), records
        // undo + selects the copy. Shared with Ctrl+D / Ctrl+V.
        if (m_selectedEntities.size() > 1 && IsEntitySelected(s_pendingDuplicate))
            DuplicateSelection();
        else
            DuplicateEntity(s_pendingDuplicate);
        s_pendingDuplicate = NullEntity;
    }

    if (s_pendingPrefabSave != NullEntity)
    {
        const Entity src = s_pendingPrefabSave;
        // SavePrefab does not honour SceneTransientTag (only SceneSerializer
        // does), so the preview stage - reachable here only with the Internal
        // toggle on - would be written out as a prefab carrying its stripped,
        // y=-4096, MainCamera-masked state.
        if (m_world->IsAlive(src) && m_world->HasComponent<PreviewStageTag>(src))
            LOG_WARNING("EditorLayer: refusing Save as Prefab on entity %u - it "
                        "belongs to the Chain Physics Preview stage (a transient, "
                        "stripped copy). Save the source prefab instead.",
                        static_cast<unsigned>(src));
        else if (m_world->IsAlive(src))
        {
            static const char* kPrefabFilter = "Prefab (*.ipfb)\0*.ipfb\0All Files\0*.*\0\0";
            const std::string savePath = SaveFileDialog(
                kPrefabFilter, "ipfb",
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!savePath.empty())
            {
                if (Resource::SavePrefab(src, *m_world, savePath))
                {
                    LOG_SUCCESS("EditorLayer: saved prefab '%s'", savePath.c_str());
                    m_needsRescan = true;
                }
                else
                    LOG_ERROR("EditorLayer: failed to save prefab '%s'", savePath.c_str());
            }
        }
        s_pendingPrefabSave = NullEntity;
    }

    // ---- Write per-part material assignments back into the source .iscn ----
    if (s_pendingIscnMatWrite != NullEntity)
    {
        const Entity root = s_pendingIscnMatWrite;
        s_pendingIscnMatWrite = NullEntity;

        const SceneSourcePath* ssp = m_world->IsAlive(root)
            ? m_world->GetComponent<SceneSourcePath>(root) : nullptr;
        if (!ssp || ssp->path.empty())
            LOG_WARNING("EditorLayer: entity %u has no SceneSourcePath — nothing to write", root);
        else
        {
            // Walk the instance's subtree and collect (meshId -> assigned .imat)
            // for every part that has both. Iterative: a .iscn subtree can be
            // 70 levels deep (see SceneInstanceLoader's own de-recursion note).
            std::vector<SceneInstanceLoader::MaterialBinding> bindings;
            std::unordered_set<uint32_t> seenMesh;
            std::vector<Entity> stack{ root };
            while (!stack.empty())
            {
                const Entity cur = stack.back();
                stack.pop_back();
                if (const Children* ch = m_world->GetComponent<Children>(cur))
                    for (Entity kid : ch->entities) stack.push_back(kid);

                const MeshLibRef*         mlr = m_world->GetComponent<MeshLibRef>(cur);
                const MaterialSourcePath* msp = m_world->GetComponent<MaterialSourcePath>(cur);
                if (!mlr || !msp || msp->path.empty()) continue;
                if (!seenMesh.insert(mlr->meshId).second) continue;

                SceneInstanceLoader::MaterialBinding b;
                b.meshId  = mlr->meshId;
                b.matPath = Sys::RelativizeAssetPath(msp->path);
                bindings.push_back(std::move(b));
            }

            int changed = 0;
            if (SceneInstanceLoader::WriteMaterialBindings(ssp->path, bindings, &changed))
            {
                if (changed > 0)
                {
                    LOG_SUCCESS("EditorLayer: wrote %d material assignment(s) into '%s'",
                                changed, ssp->path.c_str());
                    m_needsRescan = true;
                }
                else
                    LOG_INFO("EditorLayer: '%s' already names these materials — file untouched",
                             ssp->path.c_str());
            }
            else
                LOG_ERROR("EditorLayer: failed to write materials into '%s'", ssp->path.c_str());
        }
    }

    // ---- Unpack Model Instance ----
    if (s_pendingUnpackModel != NullEntity)
    {
        const Entity root = s_pendingUnpackModel;
        s_pendingUnpackModel = NullEntity;
        UnpackOutcome res;
        if (UnpackModelInstance(root, res))
            LOG_SUCCESS("EditorLayer: unpacked the model instance - %u Static Mesh(es), %u part(s) replaced, "
                        "%u collider(s) kept", res.owners, res.parts, res.colliders);
        else
            LOG_WARNING("EditorLayer: cannot unpack this model instance: %s", res.error.c_str());
    }

    // ---- Attach to Socket (context menu) ----
    // EditorLayer::AttachToSocket: a FollowSocket onto the owner's socket, the
    // item's LocalTransform zeroed - one undo step.
    if (s_pendingAttach.item != NullEntity && m_world->IsAlive(s_pendingAttach.item) &&
        m_world->IsAlive(s_pendingAttach.owner))
    {
        const std::string why = AttachToSocket(s_pendingAttach.item, s_pendingAttach.owner, s_pendingAttach.socket);
        if (!why.empty())
            LOG_WARNING("EditorLayer: cannot attach %u to socket '%s' of %u: %s", s_pendingAttach.item,
                        s_pendingAttach.socket.c_str(), s_pendingAttach.owner, why.c_str());
        s_pendingAttach = {};
    }

    // ---- UI reorder (drag onto a UI row's top / bottom edge) ----
    if (s_pendingUIReorder.child != NullEntity && m_world->IsAlive(s_pendingUIReorder.child)
        && m_world->IsAlive(s_pendingUIReorder.anchor))
    {
        const Entity c      = s_pendingUIReorder.child;
        const Entity anchor = s_pendingUIReorder.anchor;
        const Entity parent = UI::ResolveUIParent(*m_world, anchor);
        std::vector<Entity> order;
        UI::UIChildrenOf(*m_world, parent, order);
        order.erase(std::remove(order.begin(), order.end(), c), order.end());
        const auto it = std::find(order.begin(), order.end(), anchor);
        const int at = it == order.end() ? -1 : static_cast<int>(it - order.begin()) +
                                                    (s_pendingUIReorder.after ? 1 : 0);
        std::string why;
        if (parent == NullEntity || !MoveUINode(c, parent, at, &why))
            LOG_WARNING("EditorLayer: UI reorder of %u next to %u refused: %s", c, anchor,
                        parent == NullEntity ? "the row has no UI parent" : why.c_str());
        s_pendingUIReorder = {};
    }

    // ---- Reparent (drag-drop / menu), preserving the world transform ----
    if (s_pendingReparent.child != NullEntity
        && m_world->IsAlive(s_pendingReparent.child)
        && (m_world->HasComponent<UI::UIRect>(s_pendingReparent.child) ||
            UI::IsUINode(*m_world, s_pendingReparent.newParent)))
    {
        // A UI node (or anything dropped on a UI node) goes through the one UI
        // move: its refusals, its single undo step.
        std::string why;
        if (!ReparentWithUndo(s_pendingReparent.child, s_pendingReparent.newParent, &why))
            LOG_WARNING("EditorLayer: moving %u under %u refused: %s", s_pendingReparent.child,
                        static_cast<uint32_t>(s_pendingReparent.newParent), why.c_str());
        s_pendingReparent = {};
    }
    if (s_pendingReparent.child != NullEntity
        && m_world->IsAlive(s_pendingReparent.child))
    {
        const Entity c  = s_pendingReparent.child;
        const Entity np = s_pendingReparent.newParent;
        if (np != NullEntity && !m_world->IsAlive(np))
        {
            LOG_WARNING("EditorLayer: reparent target %u is dead — ignored", np);
        }
        else if (np != NullEntity && WouldCreateCycle(m_world, c, np))
        {
            LOG_WARNING("EditorLayer: reparenting %u under %u would form a cycle — ignored",
                        c, np);
        }
        else
        {
            const bool isUI = m_world->HasComponent<UI::UIParent>(c);

            // Capture pre-reparent state for undo.
            Entity         oldParent    = NullEntity;
            Entity         oldUIParent  = NullEntity;
            LocalTransform oldLocal{};
            bool           hasOldLocal  = false;
            if (isUI)
            {
                if (const UI::UIParent* up = m_world->GetComponent<UI::UIParent>(c))
                    oldUIParent = up->parent;
            }
            else
            {
                if (const Parent* p = m_world->GetComponent<Parent>(c)) oldParent = p->entity;
                if (const LocalTransform* lt = m_world->GetComponent<LocalTransform>(c))
                    { oldLocal = *lt; hasOldLocal = true; }
            }

            ReparentEntity(m_world, c, np);
            MarkHierarchyDirty();
            LOG_INFO("EditorLayer: reparented entity %u under %u",
                     c, static_cast<uint32_t>(np));

            if (UndoEnabled())
            {
                SyncedEditContext();
                if (isUI)
                {
                    PushEdit(std::make_unique<Editor::Undo::ReparentAction>(
                        m_world->MakeHandle(c), oldUIParent, np, m_world->GetName(c)));
                }
                else
                {
                    LocalTransform newLocal{};
                    bool           hasNewLocal = false;
                    if (const LocalTransform* lt = m_world->GetComponent<LocalTransform>(c))
                        { newLocal = *lt; hasNewLocal = true; }
                    PushEdit(std::make_unique<Editor::Undo::ReparentAction>(
                        m_world->MakeHandle(c), oldParent, oldLocal, hasOldLocal,
                        np, newLocal, hasNewLocal, m_world->GetName(c)));
                }
            }
        }
        s_pendingReparent = {};
    }

    // ---- Wrap Visual as Child ----
    // Moves render-only components from the parent entity into a freshly created
    // child positioned at the collider's "Snap to Bottom" Y. Keeps navigation /
    // physics state on the parent (the entity origin sits at ground level after
    // this), while the visual mesh sits half-height up.
    if (s_pendingWrapVisual != NullEntity
        && m_world->IsAlive(s_pendingWrapVisual))
    {
        const Entity parent = s_pendingWrapVisual;

        // Y offset mirrors the Snap-to-Bottom preset on ColliderComponent so
        // collider top and visual centre line up.
        float yLift = 0.0f;
        if (const ColliderComponent* col =
                m_world->GetComponent<ColliderComponent>(parent))
        {
            switch (col->shape)
            {
                case ColliderComponent::Shape::Capsule: yLift = col->halfHeight + col->radius; break;
                case ColliderComponent::Shape::Sphere:  yLift = col->radius;                   break;
                case ColliderComponent::Shape::Box:     yLift = col->halfExtents.y;            break;
                case ColliderComponent::Shape::Mesh:    yLift = 0.0f;                          break;
            }
        }

        const Entity child = m_world->CreateEntity();
        const std::string childName = m_world->GetName(parent) + "_Visual";
        m_world->SetName(child, childName);

        LocalTransform childLT;
        childLT.translation = { 0.f, yLift, 0.f };
        m_world->AddComponent<LocalTransform>(child, childLT);
        m_world->AddComponent<GlobalTransform>(child, GlobalTransform{});
        m_world->AddComponent<Parent>(child, Parent{ parent });
        if (Children* ch = m_world->GetComponent<Children>(parent))
            ch->entities.push_back(child);
        else
        {
            Children c; c.entities.push_back(child);
            m_world->AddComponent<Children>(parent, std::move(c));
        }

        // Move every render-side component the renderer cares about. The
        // collider / RigidBody / AIIntent + NavAgent stay on the parent so physics +
        // pathing keep operating from the entity origin (= ground level).
        int moved = 0;
        moved += MoveComponentBetween<MeshHandle>        (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MeshLibRef>        (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MeshSourcePath>    (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MaterialComponent> (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MaterialSourcePath>(m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<MaterialOverride>  (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<LocalAabb>         (m_world, parent, child) ? 1 : 0;
        moved += MoveComponentBetween<WorldAabb>         (m_world, parent, child) ? 1 : 0;

        m_selectedEntity    = child;
        s_pendingWrapVisual = NullEntity;
        MarkHierarchyDirty();
        LOG_INFO("EditorLayer: wrapped %d visual component(s) from entity %u into child %u (lift Y=%.3f)",
                 moved, parent, child, yLift);
    }

    // ---- Visual hint while an IPFB / Mesh drag is active ----
    const bool meshDrag = ImGui::GetDragDropPayload() &&
                          ImGui::GetDragDropPayload()->IsDataType("STATICMESH_REF");
    if (ImGui::GetDragDropPayload() &&
        (ImGui::GetDragDropPayload()->IsDataType("IPFB_PATH") || meshDrag))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        const ImVec2 bMin = win->InnerRect.Min;
        const ImVec2 bMax = win->InnerRect.Max;
        dl->AddRectFilled(bMin, bMax, IM_COL32(30, 80, 30, 60), 3.f);
        dl->AddRect(bMin, bMax, IM_COL32(70, 180, 70, 200), 3.f);
        const char* hint = meshDrag ? "Drop to place the mesh" : "Drop .ipfb to spawn prefab";
        const ImVec2 tsz = ImGui::CalcTextSize(hint);
        dl->AddText(
            ImVec2(bMin.x + (bMax.x - bMin.x - tsz.x) * 0.5f,
                   bMax.y - tsz.y - 6.f),
            IM_COL32(120, 220, 120, 230), hint);
    }

    // ---- Full-window IPFB / Static Mesh drop target ----
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (ImGui::BeginDragDropTargetCustom(win->InnerRect, win->ID + 1))
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IPFB_PATH"))
            {
                const char* pfbPath = static_cast<const char*>(p->Data);
                if (m_assetMgr && m_world)
                {
                    Entity spawned = NullEntity;
                    RecordSpawn("Spawn Prefab", [&]{
                        spawned = Resource::LoadPrefab(pfbPath, *m_world, *m_assetMgr,
                                                       m_renderer, m_animClipSys);
                        // Place the root in front of the camera (inside the record
                        // scope so the snapshot captures the final transform) - or,
                        // for a UI prefab, as a detached copy on a canvas.
                        if (spawned != NullEntity && !AdoptUIPrefabRoot(spawned, NullEntity))
                            if (auto* lt = m_world->GetComponent<LocalTransform>(spawned))
                                lt->translation = ComputeSpawnPosition();
                    });
                    if (spawned != NullEntity)
                    {
                        m_selectedEntity = spawned;
                        LOG_SUCCESS("EditorLayer: spawned prefab '%s' — root entity %u", pfbPath, spawned);
                    }
                    else
                        LOG_ERROR("EditorLayer: failed to spawn prefab '%s'", pfbPath);
                }
                else
                    LOG_ERROR("EditorLayer: assetMgr not wired — cannot spawn prefab");
            }
            // One Mesh of a model (a Content Browser Mesh item) -> a StaticMesh
            // owner in front of the camera.
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("STATICMESH_REF"))
            {
                std::string model, name;
                uint64_t    key = 0;
                if (m_world && ParseStaticMeshRef(static_cast<const char*>(p->Data), model, key, name))
                {
                    Entity e = NullEntity;
                    RecordSpawn("Place Mesh", [&] {
                        e = CreateStaticMeshEntity(model, key, name, ComputeSpawnPosition());
                    });
                    if (e != NullEntity)
                    {
                        m_selectedEntity = e;
                        MarkHierarchyDirty();
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::End();
}

// Shared authoring operation: called only from the main thread at a safe point.
size_t EditorLayer::MoveEntitiesToFolder(const std::vector<Entity>& targets,
                                       const std::string& folderPath, bool resolveRoots)
{
    if (!m_world || GetPlayState() != ViewportPlayState::Stopped) return 0;
    HierarchyMap hmap;
    BuildHierarchyMap(m_world, hmap, /*hideMeshSections=*/false);   // the true parents, every entity
    std::unordered_set<Entity> roots;
    for (Entity e : targets)
    {
        if (!m_world->IsAlive(e)) continue;
        std::unordered_set<Entity> seen;
        for (int depth = 0; resolveRoots && depth < 512; ++depth)
        {
            const Entity parent = hmap.ParentOf(e);
            if (parent == NullEntity || !seen.insert(parent).second) break;
            e = parent;
        }
        roots.insert(e);
    }
    const std::string path = folderPath;
    auto restore = [](std::string folder) -> Editor::Undo::RestoreFn {
        return [folder = std::move(folder)](World& world, Entity e) {
            if (folder.empty()) world.RemoveComponent<EditorFolderComponent>(e);
            else if (auto* f = world.GetComponent<EditorFolderComponent>(e)) f->path = folder;
            else world.AddComponent<EditorFolderComponent>(e, EditorFolderComponent{folder});
        };
    };
    if (UndoEnabled()) { SyncedEditContext(); m_editHistory.BeginMacro("Move entities to folder"); }
    size_t changed = 0;
    for (Entity e : roots)
    {
        const auto* old = m_world->GetComponent<EditorFolderComponent>(e);
        const std::string before = old ? old->path : "";
        if (before == path) continue;
        restore(path)(*m_world, e);
        ++changed;
        if (UndoEnabled())
            PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(
                m_world->MakeHandle(e), restore(before), restore(path), "Folder"));
    }
    if (UndoEnabled()) m_editHistory.EndMacro();
    if (!path.empty()) m_editorFolders.insert(path);
    if (changed)
    {
        MarkHierarchyDirty();
        // In Edit mode every move above is in the macro; otherwise nothing can take it back.
        if (UndoEnabled()) SyncSceneDirtyToHistory();
        else               MarkSceneDirty();
        m_hierarchyRevealEntity = m_selectedEntity;
    }
    return changed;
}

std::string EditorLayer::ExecuteEditorCommand(const std::string& request)
{
    using Json = nlohmann::json;
    auto error = [](const char* message) { return Json{{"ok",false},{"error",message}}.dump(); };
    try
    {
        const Json cmd = request.empty() ? Json{{"op","help"}} : Json::parse(request);
        if (!cmd.is_object()) return error("request_must_be_object");
        const std::string op = cmd.value("op", "help");
        if(op=="attack")return AttackEditor::Control(request);
        if(op=="animgraph")return Editor::AnimGraphEditor::Control(request);
        if (op == "help")
            return Json{{"ok",true},{"ops",{"help","state","list","selection","select","pick","focus","move-folder","undo","redo","save","attack","animgraph"}},
                        {"selectModes",{"replace","add","toggle","range"}},
                        {"save","Queued at a safe frame boundary: the reply's jobId is the result (`job wait <id>`); state.lastSaveResult is the last save of any kind"}}.dump();
        if (!m_world) return error("no_world");
        ReconcileSelection();
        auto entityFrom = [&](const Json& value) -> Entity {
            if (!value.is_number_integer()) throw std::runtime_error("entity_id_must_be_integer");
            const int64_t id = value.get<int64_t>();
            if (id <= 0 || id > UINT32_MAX || !m_world->IsAlive(static_cast<Entity>(id)))
                throw std::runtime_error("entity_not_alive");
            return static_cast<Entity>(id);
        };
        auto selection = [&]() {
            Json ids = Json::array();
            for (Entity e : m_selectedEntities) { if (ids.size() == 512) break; ids.push_back(e); }
            return Json{{"primary",m_selectedEntity},{"count",m_selectedEntities.size()},{"ids",ids},
                        {"truncated",m_selectedEntities.size() > ids.size()}};
        };
        auto reveal = [&]() {
            m_hierarchyRevealEntity = m_selectedEntity;
            m_hierarchyHighlight = m_selectedEntity;
            m_lastSeenSelectedEntity = m_selectedEntity;
            MarkHierarchyDirty();
        };
        if (op == "state")
            return Json{{"ok",true},{"selection",selection()},{"scene",m_currentScenePath},
                        {"dirty",m_sceneDirty},{"savePending",!m_pendingSaveScenePath.empty()},
                        {"lastSaveResult",m_lastSaveResult},{"canUndo",m_editHistory.CanUndo()},
                        {"canRedo",m_editHistory.CanRedo()},
                        {"editMode",GetPlayState() == ViewportPlayState::Stopped}}.dump();
        if (op == "selection") return Json{{"ok",true},{"selection",selection()}}.dump();
        if (op == "list")
        {
            const std::string filter = cmd.value("filter", "");
            const int offset = cmd.value("offset", 0), limit = cmd.value("limit", 100);
            if (offset < 0 || limit < 1 || limit > 128) return error("invalid_page_range");
            const bool internal = cmd.value("internal", false);
            Json rows = Json::array();
            size_t total = 0;
            for (Entity e : m_world->GetEntities())
            {
                if (!m_world->IsAlive(e)) continue;
                // The Chain Physics Preview's stage is hidden here exactly as in
                // the Outliner.
                if (!internal && (m_world->HasComponent<EditorCameraTag>(e)
                    || m_world->HasComponent<CameraStackChannelSingleton>(e)
                    || m_world->HasComponent<PreviewStageTag>(e))) continue;
                const auto& name = m_world->GetName(e);
                if (name.find(filter) == std::string::npos) continue;
                const size_t index = total++;
                if (index < static_cast<size_t>(offset) || rows.size() >= static_cast<size_t>(limit)) continue;
                const auto* folder = m_world->GetComponent<EditorFolderComponent>(e);
                const auto* parent = m_world->GetComponent<Parent>(e);
                Json row{{"id",e},{"name",name},{"parent",parent ? parent->entity : NullEntity},
                         {"folder",folder ? folder->path : ""},{"selected",IsEntitySelected(e)},
                         {"prefab",m_world->HasComponent<PrefabSourcePath>(e)},
                         {"mesh",m_world->HasComponent<MeshComponent>(e) || m_world->HasComponent<MeshLibRef>(e)
                            || m_world->HasComponent<MeshHandle>(e)},
                         {"editorCamera",m_world->HasComponent<EditorCameraTag>(e)}};
                if (const auto* tr = m_world->GetComponent<LocalTransform>(e))
                    row["position"] = {tr->translation.x,tr->translation.y,tr->translation.z};
                rows.push_back(std::move(row));
            }
            Json result{{"ok",true},{"entities",rows},{"total",total},{"offset",offset},
                        {"nextOffset",static_cast<size_t>(offset)+rows.size()}};
            const std::string serialized = result.dump();
            if (serialized.size() > 48000) return error("response_too_large_reduce_limit");
            return serialized;
        }
        if (op == "select")
        {
            const std::string mode = cmd.value("mode", "replace");
            if (mode != "replace" && mode != "add" && mode != "toggle" && mode != "range")
                return error("invalid_select_mode");
            const auto& values = cmd.at("ids");
            if (!values.is_array() || values.size() > 512) return error("ids_must_be_array_max_512");
            if (mode == "range" && values.size() != 1) return error("range_requires_one_id");
            std::vector<Entity> ids;
            for (const auto& value : values)
            {
                const Entity e = entityFrom(value);
                if (std::find(ids.begin(), ids.end(), e) == ids.end()) ids.push_back(e);
            }
            // A preview-stage id is refused (SetPickResult drops it too), checked
            // for the whole request before anything changes.
            for (Entity e : ids)
                if (m_world->IsAlive(e) && m_world->HasComponent<PreviewStageTag>(e))
                    return error("preview_stage_entity");
            // Validate the complete request BEFORE changing the current selection.
            if (mode == "replace") ClearEntitySelection();
            for (Entity e : ids)
                SelectEntity(e, mode == "toggle" ? SelectMode::Toggle : mode == "range" ? SelectMode::Range : SelectMode::Add);
            reveal();
            return Json{{"ok",true},{"selection",selection()}}.dump();
        }
        if (op == "pick")
        {
            const Entity e = entityFrom(cmd.at("id"));
            if (m_world->IsAlive(e) && m_world->HasComponent<PreviewStageTag>(e))
                return error("preview_stage_entity");
            m_pickSelectMode = SelectMode::Replace;
            SetPickResult(e);
            return Json{{"ok",true},{"selection",selection()}}.dump();
        }
        if (op == "focus")
        {
            const Entity e = cmd.contains("id") ? entityFrom(cmd.at("id")) : m_selectedEntity;
            if (e == NullEntity || !m_world->HasComponent<GlobalTransform>(e)) return error("no_focus_target");
            if (cmd.contains("id")) SelectEntity(e, SelectMode::Replace);
            FocusSelectedEntity();
            reveal();
            return Json{{"ok",true},{"primary",m_selectedEntity}}.dump();
        }
        if (GetPlayState() != ViewportPlayState::Stopped) return error("requires_edit_mode");
        if (op == "move-folder")
        {
            std::string path = cmd.at("path").get<std::string>();
            if (path.size() > 1024) return error("folder_path_too_long");
            std::replace(path.begin(), path.end(), '\\', '/');
            if (!path.empty() && (path.front() == '/' || path.back() == '/' || path.find("//") != std::string::npos))
                return error("folder_path_must_be_relative");
            for (unsigned char c : path) if (c < 32) return error("invalid_folder_character");
            std::istringstream segments(path); std::string segment;
            while (std::getline(segments, segment, '/'))
                if (segment == "." || segment == "..") return error("invalid_folder_segment");
            if (m_selectedEntities.empty()) return error("empty_selection");
            const size_t changed = MoveEntitiesToFolder(m_selectedEntities, path);
            return Json{{"ok",true},{"changedRoots",changed},{"folder",path}}.dump();
        }
        if (op == "undo" || op == "redo")
        {
            if (op == "undo" && !m_editHistory.CanUndo()) return error("nothing_to_undo");
            if (op == "redo" && !m_editHistory.CanRedo()) return error("nothing_to_redo");
            if (op == "undo") EditUndo(); else EditRedo();
            reveal();
            return Json{{"ok",true}}.dump();
        }
        if (op == "save")
        {
            if (!m_pendingSaveScenePath.empty())
            {
                Json busy{{"ok",false},{"error","save_already_pending"}};
                if (m_pendingSaveJobId) busy["jobId"] = m_pendingSaveJobId;   // wait on that one
                return busy.dump();
            }
            const std::string path = cmd.value("path", m_currentScenePath);
            if (path.empty() || !path.ends_with(".iscene")) return error("iscene_path_required");
            const auto saveJob = Harness::Jobs::TryCreate("save_scene", path, Logger::CurrentRequestId());
            if (!saveJob) return error("busy");
            if (!RequestSaveScene(path)) {
                Harness::Jobs::Fail(saveJob, "save_refused", "save request refused", {});
                return error("save_refused");
            }
            // The write happens at the next frame boundary: queued is not saved.
            // `job wait <jobId>` returns succeeded (path, bytes) or failed (code,
            // message, the lines the write logged).
            m_pendingSaveRequestId = Logger::CurrentRequestId();
            m_pendingSaveJobId = saveJob;
            Json queued{{"ok",true},{"status","queued"},{"path",path}};
            if (m_pendingSaveJobId) queued["jobId"] = m_pendingSaveJobId;
            return queued.dump();
        }
        return error("unknown_editor_op");
    }
    catch (const std::exception& e)
    {
        return Json{{"ok",false},{"error","invalid_request"},{"detail",e.what()}}.dump();
    }
}
