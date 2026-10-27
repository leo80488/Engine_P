#pragma once

// ChainParamTree.h - the seam between the Chain Physics parameter tree
// (EditorLayer::DrawChainParamTree, in ChainPhysicsEditorWindow.cpp) and its one
// caller, the Chain Physics Preview panel - the one place chains are edited:
//   * EDIT mode: the panel passes its m_staged copy and hooks.invalidate only
//     RAISES A FLAG; the panel's ProcessPendingActions is the one writer of the
//     stage's live component (field-wise CopyAuthoredInto, never a whole-struct
//     copy) and re-initialises there, in the serial window. The tree writes no
//     World or renderer state.
//   * PLAY mode: the Parameters section is "LIVE: <scene entity>", drawn by an
//     EditorLayer functor (ChainLiveEditFn) that passes the scene entity's LIVE
//     component inside DrawComponentWithUndo (no global undo in Play) with
//     hooks.invalidate = ChainPhysicsSystem::Invalidate; transient (Stop's
//     AdoptState reverts it).

#include "ECS/ECS.h"   // Entity

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ChainPhysicsComponent;
struct SkeletonAsset;

// The authored-rest-pose tools' state (the "Pose bones" toggle, the picked
// bone, the focus group) is PER TREE, never one EditorLayer global: a bone
// INDEX is meaningful for one skeleton only. `owner` is the entity the state
// belongs to; the rest-pose section resets the state whenever it is drawn for
// a different one.
struct ChainPoseState
{
    bool          poseMode   = false;
    std::uint32_t poseBone   = ~0u;           // ~0u = none
    int           focusGroup = -1;            // -1 = every group
    Entity        owner      = NullEntity;
    std::uint32_t skelAsset  = 0xFFFFFFFFu;   // the skeleton poseBone indexes
    // Drops everything that names a bone of the previous owner.
    void Reset(Entity newOwner)
    {
        poseMode = false; poseBone = ~0u; focusGroup = -1;
        owner = newOwner; skelAsset = 0xFFFFFFFFu;
    }
};

struct ChainParamHooks
{
    // The entity the tree describes: the rest-pose section reads its LocalPose
    // (seeding an entry) and asks ChainPhysicsSystem::IsSimulating(entity).
    Entity entity = NullEntity;
    // Called by every STRUCTURAL control (root bone, excludes, group add /
    // remove / enable, ring / pair ids, the authored rest pose). Live controls
    // never call it.
    std::function<void()> invalidate;
    // Draw the re-initialise marker beside the structural controls (the panel's
    // staged tree; the Play live tree draws none).
    bool markStructural = false;
    // Paint the REFERENCE BANDS (typical range + PBD-saturation veil,
    // Editor::ChainPresets::DrawBand) onto the material sliders, and the reflected
    // globals' band table. The panel's staged drawers only; the Play live tree
    // draws none.
    bool referenceBands = false;
    // The active preset's covered keys (a blue chip on those fields); may be null.
    const std::vector<std::string>* coveredFields = nullptr;
    // This tree's rest-pose tool state. Null = an EditorLayer scratch state keyed
    // by `entity`.
    ChainPoseState* pose = nullptr;
    // `entity` is what the MAIN viewport's chain overlay draws (the selection).
    // Only then may the tree write the main overlay's highlight group or switch
    // that overlay on - otherwise an expanded group of the preview's STAGE dims an
    // unrelated scene character.
    bool mainOverlayTarget = false;
    // > 0 = the tree is drawn into a column whose width the user controls: every
    // slider gets PushItemWidth(-labelColumn) (the label keeps that many px on the
    // right; PushItemWidth, not SetNextItemWidth, so ChainPresets::DrawBand's
    // CalcItemWidth reads the same width back), the rest pose's fixed widths become
    // width-aware, the root picker fits the popup, and REFLECT_INFO / long
    // read-outs wrap. 0 = fixed default widths.
    float labelColumn = 0.f;
    // Harness -chainpreview_sections=...tree: open every header and group node of
    // the tree (the exclude bone trees stay closed); the layout pack measures the
    // fully expanded tree.
    bool expandAll = false;
};

// What EditorLayer hands the preview panel so it can draw the SAME tree on its
// staged copy without including EditorLayer.h.
using ChainParamTreeFn = std::function<void(ChainPhysicsComponent&, const SkeletonAsset&,
                                            const ChainParamHooks&)>;
// The Play-mode "LIVE" Parameters body. It reads and writes the World (a
// scene entity's live component), so it is EditorLayer code the panel only
// CALLS; the panel's Draw stays World-free.
using ChainLiveEditFn = std::function<void()>;
