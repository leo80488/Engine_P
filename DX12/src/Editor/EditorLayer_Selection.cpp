// EditorLayer: entity multi-selection.
//
// Invariant: m_selectedEntity is the PRIMARY and always equals m_selectedEntities.back()
// (NullEntity when the selection is empty). m_selectedEntity stays a plain member: its many
// assignment sites across the editor each mean "select exactly this, or nothing", so the
// per-frame ReconcileSelection() detects a direct assignment and collapses the multi-selection to
// match. SelectEntity() updates both, so it never trips the collapse.

#include "Editor/EditorLayer.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/StaticMeshComponents.h"   // a section stands for its StaticMesh owner
#include "Editor/Undo/ComponentBlob.h"
#include "Editor/Undo/ComponentEditAction.h"
#include "System/Log.h"

#include <algorithm>
#include <sstream>

namespace
{
    // Hierarchy walks are bounded rather than trusted: a Parent cycle should be impossible (the
    // reparent path rejects them), but one inside a per-frame UI loop would be a hang.
    constexpr int kMaxHierarchyDepth = 512;
}

Entity EditorLayer::ViewportSelectionRoot(Entity hit) const
{
    if (!m_world || !m_world->IsAlive(hit)) return NullEntity;
    // A derived StaticMesh section stands for its owner (never selected itself).
    hit = SelectableEntity(hit);
    if (const auto* member = m_world->GetComponent<PrefabInstanceMember>(hit);
        member && m_world->IsAlive(member->root)
        && m_world->HasComponent<PrefabSourcePath>(member->root))
        return member->root;

    Entity root = hit;
    Entity model = NullEntity;
    Entity authoredNode = NullEntity;
    for (int depth = 0; depth < kMaxHierarchyDepth; ++depth)
    {
        if (m_world->HasComponent<PrefabSourcePath>(root)) return root;
        // Imported scenes can contain a whole campus: select the nearest authored node owning the
        // mesh, not the shared scene container. A StaticMesh owner is such a node too (it is the placed
        // object).
        if (authoredNode == NullEntity && (m_world->HasComponent<SceneNodeTag>(root) ||
                                           m_world->HasComponent<StaticMeshComponent>(root)))
            authoredNode = root;
        if (model == NullEntity && m_world->HasComponent<SceneSourcePath>(root)) model = root;
        const auto* parent = m_world->GetComponent<Parent>(root);
        if (!parent || parent->entity == NullEntity || !m_world->IsAlive(parent->entity)) break;
        root = parent->entity;
    }
    if (authoredNode != NullEntity) return authoredNode;
    return model != NullEntity ? model : root;
}

bool EditorLayer::IsEntitySelected(Entity e) const
{
    return e != NullEntity
        && std::find(m_selectedEntities.begin(), m_selectedEntities.end(), e)
               != m_selectedEntities.end();
}

void EditorLayer::ClearEntitySelection()
{
    m_selectedEntities.clear();
    m_selectedEntity       = NullEntity;
    m_selectionRangeAnchor = NullEntity;
}

void EditorLayer::SelectEntity(Entity e, SelectMode mode)
{
    if (e == NullEntity)
    {
        if (mode == SelectMode::Replace) ClearEntitySelection();
        return;
    }

    switch (mode)
    {
    case SelectMode::Replace:
        m_selectedEntities.assign(1, e);
        m_selectionRangeAnchor = e;
        break;

    case SelectMode::Add:
        // Already selected: re-seat it at the back so it becomes the primary (Ctrl+click on a selected
        // row moves the Inspector to it).
        std::erase(m_selectedEntities, e);
        m_selectedEntities.push_back(e);
        m_selectionRangeAnchor = e;
        break;

    case SelectMode::Toggle:
        if (IsEntitySelected(e)) std::erase(m_selectedEntities, e);
        else                     m_selectedEntities.push_back(e);
        m_selectionRangeAnchor = e;
        break;

    case SelectMode::Range:
    {
        // Range runs over the flattened row order the Hierarchy just drew: what is visually between
        // two rows only the traversal knows, not the parent/child links.
        const auto& order = m_hierarchyRowOrder;
        const auto ia = std::find(order.begin(), order.end(), m_selectionRangeAnchor);
        const auto ib = std::find(order.begin(), order.end(), e);
        if (ia == order.end() || ib == order.end())
        {
            // No usable anchor (first click, or the anchor row is collapsed out of view): degrade to a
            // plain select rather than guess.
            m_selectedEntities.assign(1, e);
            m_selectionRangeAnchor = e;
            break;
        }
        auto lo = ia, hi = ib;
        if (lo > hi) std::swap(lo, hi);
        m_selectedEntities.assign(lo, hi + 1);
        // The clicked row is the primary, not whichever end sorted last.
        std::erase(m_selectedEntities, e);
        m_selectedEntities.push_back(e);
        // The anchor stays: shift-clicking again re-extends from the original anchor, not from the
        // far end of this range.
        break;
    }
    }

    m_selectedEntity = m_selectedEntities.empty() ? NullEntity
                                                  : m_selectedEntities.back();
}

std::vector<Entity> EditorLayer::SelectionRoots() const
{
    std::vector<Entity> roots;
    if (!m_world) return roots;
    roots.reserve(m_selectedEntities.size());

    for (Entity e : m_selectedEntities)
    {
        bool carriedByAncestor = false;
        Entity walk = e;
        for (int depth = 0; depth < kMaxHierarchyDepth; ++depth)
        {
            const Parent* par = m_world->GetComponent<Parent>(walk);
            if (!par || par->entity == NullEntity) break;
            walk = par->entity;
            if (IsEntitySelected(walk)) { carriedByAncestor = true; break; }
        }
        if (!carriedByAncestor) roots.push_back(e);
    }
    return roots;
}

void EditorLayer::ReconcileSelection()
{
    if (!m_world)
    {
        m_selectedEntities.clear();
        return;
    }

    // Prune first. The World's entity-destroy listener nulls m_selectedEntity when the primary
    // dies, which looks like an explicit deselect except that a member also vanished; reconciling
    // before the prune would wipe the whole selection when one member is destroyed.
    const std::size_t removed = std::erase_if(
        m_selectedEntities, [this](Entity e) { return !m_world->IsAlive(e); });

    const Entity vecPrimary = m_selectedEntities.empty() ? NullEntity
                                                         : m_selectedEntities.back();
    if (m_selectedEntity != vecPrimary)
    {
        const bool primaryAlive = m_selectedEntity != NullEntity
                               && m_world->IsAlive(m_selectedEntity);
        if (primaryAlive)
        {
            // A direct assignment (create, duplicate, viewport pick, scene load): each such site means
            // one entity, so collapse rather than let the two views drift.
            m_selectedEntities.assign(1, m_selectedEntity);
        }
        else if (removed > 0)
        {
            // The primary was destroyed and the listener nulled it. Keep the
            // survivors and hand the primary to one of them.
            m_selectedEntity = vecPrimary;
        }
        else
        {
            // Nulled with nothing dying: an explicit "select nothing".
            m_selectedEntities.clear();
        }
    }

    if (!IsEntitySelected(m_selectionRangeAnchor))
        m_selectionRangeAnchor = m_selectedEntity;
}

// ==== Multi-selection field editing ====
// The Inspector always draws the PRIMARY. When a field edit commits, the keys that changed are
// pushed onto every other selected entity that has the same component: "edit applies to all"
// without an N-way Inspector.

namespace
{
    // A component blob is the serializer's "  Tag: k=v k=v\n" line. Returns that line's key/value
    // pairs, nothing else.
    std::vector<std::pair<std::string, std::string>> BlobPairs(const std::string& line)
    {
        std::vector<std::pair<std::string, std::string>> out;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) return out;
        std::istringstream tok(line.substr(colon + 1));
        std::string t;
        while (tok >> t)
        {
            const std::size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            out.emplace_back(t.substr(0, eq), t.substr(eq + 1));
        }
        return out;
    }

    // Count of non-empty '\n'-separated lines. Multi-line blobs (ScriptComponent emits one line per
    // slot) have no unambiguous key identity across lines, so propagation refuses them rather than
    // corrupt a slot list.
    std::size_t BlobLineCount(const std::string& blob)
    {
        std::size_t n = 0;
        std::istringstream ls(blob);
        std::string line;
        while (std::getline(ls, line))
            if (line.find_first_not_of(" \t\r") != std::string::npos) ++n;
        return n;
    }

    // Returns `line` with every key in `overrides` set to its value. Keys absent from the line are
    // appended, so a component whose serializer omits a default-valued field still receives the
    // edit.
    std::string ApplyOverrides(const std::string& line,
                               const std::vector<std::pair<std::string, std::string>>& overrides)
    {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) return line;

        std::vector<std::pair<std::string, std::string>> pairs = BlobPairs(line);
        for (const auto& [k, v] : overrides)
        {
            auto it = std::find_if(pairs.begin(), pairs.end(),
                                   [&k](const auto& p) { return p.first == k; });
            if (it != pairs.end()) it->second = v;
            else                   pairs.emplace_back(k, v);
        }

        std::string out = line.substr(0, colon + 1);
        for (const auto& [k, v] : pairs) { out += ' '; out += k; out += '='; out += v; }
        out += '\n';
        return out;
    }
}

void EditorLayer::PropagateFieldEditToSelection(Entity primary, std::type_index type,
                                                const std::string& blobBefore,
                                                const std::string& blobAfter)
{
    if (!m_world || m_selectedEntities.size() <= 1)      return;
    if (blobBefore.empty() || blobAfter.empty())         return;
    if (blobBefore == blobAfter)                         return;
    // See BlobLineCount: a multi-slot component has no stable key identity.
    if (BlobLineCount(blobBefore) != 1 || BlobLineCount(blobAfter) != 1) return;

    ComponentSerializerRegistry& reg = GetComponentRegistry();
    if (!Editor::Undo::HasSerializer(reg, type)) return;

    // Only the keys the user changed: copying the whole blob would flatten every other field of
    // the selection onto the primary's values.
    const auto beforePairs = BlobPairs(blobBefore);
    const auto afterPairs  = BlobPairs(blobAfter);
    std::vector<std::pair<std::string, std::string>> changed;
    for (const auto& [k, v] : afterPairs)
    {
        const auto it = std::find_if(beforePairs.begin(), beforePairs.end(),
                                     [&k](const auto& p) { return p.first == k; });
        if (it == beforePairs.end() || it->second != v) changed.emplace_back(k, v);
    }
    if (changed.empty()) return;

    std::size_t applied = 0;
    for (Entity other : m_selectedEntities)
    {
        if (other == primary || !m_world->IsAlive(other)) continue;

        const std::string otherBefore =
            Editor::Undo::SerializeComponentBlob(reg, *m_world, other, type);
        // Empty: this entity lacks the component (not a common field). Skip it rather than create the
        // component on it.
        if (otherBefore.empty() || BlobLineCount(otherBefore) != 1) continue;

        const std::string otherAfter = ApplyOverrides(otherBefore, changed);
        if (otherAfter == otherBefore) continue;

        // Rewrite the other entity's live component in place. Do not plain-deserialize: that resets
        // its runtime state (with several lights selected, each other one would be re-stamped as the
        // newest light, and a directional one would become the sun) and appends a second slot to a
        // one-script entity.
        if (!ComponentWrite::RewriteFromBlob(reg, *m_world, other, type, otherAfter, m_assetMgr))
            continue;

        // Inside the caller's macro, so all of this is one Ctrl+Z.
        PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(
            m_world->MakeHandle(other), type, otherBefore, otherAfter,
            ComponentLabelFor(type)));
        ++applied;
    }

    if (applied > 0)
        LOG_INFO("Inspector: applied %s edit to %zu more selected entit%s",
                 ComponentLabelFor(type), applied, applied == 1 ? "y" : "ies");
}
