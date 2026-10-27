#pragma once

// HumanoidMapping - the humanoid role map's editing pieces, shared by the Animation
// Retarget window and the Skeleton Editor's Retarget tab. Both edit a
// SkeletonAsset::humanoid the same way:
//   * a role changed by hand: userEdited = 1, HumanoidProfileBuilder::FinalizeDerived;
//   * Auto-Detect: userEdited = 0, HumanoidProfileBuilder::Build;
//   * Clear All: Reset, userEdited = 1, FinalizeDerived;
// and save it the one way an .iskel is written after its import: the humanoid
// section at the END of the payload, patched in place (ISKEL_FLAG_HUMANOID set) or
// appended (the flag set, dataSize grown).
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "Resource/HumanoidBone.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

struct SkeletonAsset;

namespace Editor
{
    // Write @p hp into @p path's humanoid section (see above). Atomic (a temporary and
    // a rename): a reader sees the old file or the new one. Refused (false + @p why)
    // for a file that is not an .iskel, one whose flagged section is too short, and one
    // reached through a reparse point (another checkout's content, asset\Bistro).
    bool SaveHumanoidProfileToIskel(const std::string& path, const HumanoidProfile& hp, std::string* why = nullptr);

    // One mapping row: the role's label, then a button naming its bone that opens a
    // searchable bone list. True when the assignment changed. @p noneLabel names -1.
    // @p labelClicked (optional): the label is a Selectable, and a click on it sets
    // *labelClicked (the Skeleton Editor selects the role's bone).
    bool HumanoidBonePickerRow(const char* label, bool required, const SkeletonAsset& sk, std::int32_t& ioBone,
                               char* filter, std::size_t filterCap, const char* noneLabel = "(none)",
                               bool* labelClicked = nullptr);

    // HumanoidConvention's name ("Mixamo", "PMX (JP)", ...; "?" out of range).
    const char* HumanoidConventionName(std::uint8_t convention);

    // The roles in the order both panels list them (Unity's Avatar groups).
    struct HumanoidRoleGroup
    {
        const char*         name;
        const HumanoidBone* roles;
        std::size_t         count;
        bool                defaultOpen;
    };
    // Torso / Head, Left Arm, Right Arm, Left Leg, Right Leg, Left Fingers, Right Fingers.
    const HumanoidRoleGroup* HumanoidRoleGroups(std::size_t& count);

    // Every group as a tree of picker rows, and the Root Motion row (hipsTranslation)
    // after the torso; a change is applied as a hand edit (userEdited = 1,
    // FinalizeDerived). @p onRoleClicked (optional): a role's label is clickable and
    // names its bone (-1 = unmapped). @p beforeChange (optional): called once, before
    // the first change this call applies (the Skeleton Editor's undo step).
    // True when the profile changed.
    bool DrawHumanoidRoleRows(SkeletonAsset& sk, char* filter, std::size_t filterCap,
                              const std::function<void(HumanoidBone role, int bone)>& onRoleClicked = {},
                              const std::function<void()>& beforeChange = {});
}
