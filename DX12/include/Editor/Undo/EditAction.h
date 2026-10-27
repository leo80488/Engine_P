#pragma once

// Editor Undo/Redo - Command Pattern for editor operations.
//
// This is the EDITOR-side action-recording layer. It is deliberately NOT
// related to:
//   * ECS `CommandBuffer` (ECS/CommandBuffer.h) - a per-phase deferred
//     structural-mutation queue for parallel systems, and
//   * the `Command::` namespace (ECS/CommandAPI.h) - the Lua -> engine facade.
// Keep those concerns separate; everything here lives in `Editor::Undo`.
//
// One user gesture = one IEditAction on the EditHistory stack. An action
// records enough state to both Apply (do / redo) and Revert (undo) the edit.
// Continuous edits (a whole gizmo drag, a slider scrub) are coalesced into a
// single action by the call site before they are pushed (see EditorLayer's
// transform-edit bracketing), so the stack gets one entry per gesture.

#include "ECS/ECS.h"   // Entity, World

#include <functional>
#include <vector>

class ComponentSerializerRegistry;
namespace Resource { class AssetManager; }

namespace Editor::Undo
{
// Restores a previously-captured component value onto (world, entity).
using RestoreFn = std::function<void(World&, Entity)>;
// Snapshots a component's CURRENT value and returns a RestoreFn that puts
// exactly that value back later. Deep-copies by value, so it is correct for
// components holding std::string / std::vector / std::unordered_map (unlike a
// raw memcpy) and for POD alike. Empty RestoreFn if the component is absent.
using CaptureFn = std::function<RestoreFn(World&, Entity)>;

// Everything an action needs to touch the editor state. Passed by reference to
// Apply/Revert so actions stay free of global state and are unit-testable.
// Kept a plain struct on purpose.
struct EditContext
{
    World*                       world  = nullptr;
    // Serializer registry + asset manager for blob-based component actions
    // (inspector field edits, remove-component data restore). May be null for
    // transform-only usage.
    ComponentSerializerRegistry* reg    = nullptr;
    Resource::AssetManager*      assets = nullptr;
    // Structural-action GPU-slot lifetime hooks (all may be null):
    //   onEntitiesWillDetach - BEFORE destroying a subtree, so the renderer can
    //                          park its GPU descriptor slots (keep them alive).
    //   onEntitiesRestored   - AFTER re-attaching, so the renderer un-parks /
    //                          re-resolves GPU state for the restored entities.
    //   onEntitiesDiscarded  - the action was dropped while detached; release
    //                          the parked slots for good.
    std::function<void(const std::vector<Entity>&)> onEntitiesWillDetach;
    std::function<void(const std::vector<Entity>&)> onEntitiesRestored;
    std::function<void(const std::vector<Entity>&)> onEntitiesDiscarded;
    // Set the editor selection when a structural action changes what exists
    // (resurrect -> select the root; delete -> clear). May be null.
    std::function<void(Entity)> setSelection;
};

// One reversible editor edit. Implementations capture their own before/after
// state at construction time.
class IEditAction
{
public:
    virtual ~IEditAction() = default;

    // Apply the edit forward (initial do is usually performed by the call site
    // that also constructs the action; Apply is what Redo calls).
    virtual void Apply(EditContext&) = 0;

    // Undo the edit: restore the pre-edit state.
    virtual void Revert(EditContext&) = 0;

    // Human-readable label for the Edit menu ("Undo Transform").
    virtual const char* Label() const = 0;

    // Fold a just-produced, adjacent same-kind edit into this one so repeated
    // continuous commits collapse to a single undo step. Return true if merged
    // (the incoming action is then discarded). Default: never merge.
    virtual bool TryMerge(const IEditAction& /*incoming*/) { return false; }

    // Called by EditHistory when this action is permanently dropped (trimmed
    // past the budget, or cleared) WITHOUT being undone/redone first. Lets an
    // action release resources it was holding, e.g. a delete action releasing
    // the parked entity ids it reserved for a possible resurrection. Default:
    // nothing to release.
    virtual void OnDiscard(EditContext& /*ctx*/) {}

    // Approximate heap footprint, for the history byte-budget. 0 for POD-only
    // actions (their cost is the fixed sizeof, accounted separately).
    virtual size_t HeapBytes() const { return 0; }

    // Made while the control channel served a request: an AI agent's edit, not
    // the person's (EditHistory's AgentEditAction). Kept apart from Label() so
    // the label stays the edit's name for scripts and tests that read it; the
    // Edit menu shows the origin.
    virtual bool ByAgent() const { return false; }
};

} // namespace Editor::Undo
