#pragma once

// MaterialSlotRow - one material slot drawn as one Inspector row: a thumbnail
// (the Content Browser's tile: the file on disk), the asset field (drop an
// IMAT_PATH, type a path + Enter, Browse..., Clear), the type tag, a one-line
// summary, the parent and the per-entity override count, the Render.explain
// warnings, and [Edit] - the editing itself happens in the Material Editor.
// The entity's Material section and every Static Mesh slot draw this widget.
//
// It changes nothing itself: it returns the action, and the caller
// (EditorLayer::RunMaterialRowAction) carries it out - the one handler
// Editor.InspectorMaterialAction runs too, so a script proves what a click
// does. The dialogs (Browse, Save as Asset, Promote) belong to the handler for
// the same reason: a script passes the path instead.
//
// Editor-only TU: compiled into EditorCore.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct MaterialComponent;

namespace Editor
{
    struct MaterialRowInfo
    {
        std::string   path;                    // the asset; "" = none (an inline material)
        std::string   tag;                     // "Standard" | "Custom HLSL" | "Graph" | "Instance" | "Inline"
        std::string   summary;                 // e.g. "Opaque - Default Lit", "Alpha - Unlit - Decal" (joined by U+00B7)
        std::string   parent;                  // an instance's parent; "" = none
        bool          parentResolved  = true;
        int           entityOverrides = -1;    // the entity's MaterialOverride props; -1 = not an entity row
        bool          unsaved         = false; // edits made on the entity and saved nowhere yet
        const char*   slotState       = nullptr; // a Static Mesh slot: "override" | "default"; nullptr = entity row
        std::vector<std::pair<std::string, std::string>> warnings;   // Render.explain causes: (code, text)
        std::uint64_t thumb           = 0;     // the tile (an ImTextureID); 0 = the placeholder
    };

    enum class MaterialRowAction : std::uint8_t
    {
        None,
        Assign,            // result.path: dropped, typed, or picked by Browse
        Browse,            // the handler asks for a file, then assigns it
        Clear,             // entity: the engine's default material; slot: the model's default
        Edit,              // the asset in the Material Editor
        EditHere,          // the entity's own material in the full panel below the row
        CreateInstance,    // a new .imatinst of the asset: assigned, opened in the Material Editor
        Find,              // the asset in the Content Browser
        CopyPath,
        SaveAsAsset,       // an inline material written to a new .imat (result.path, or a dialog), assigned
        PromoteOverrides,  // the entity's material as a new .imatinst of its asset (result.path, or a dialog)
        ClearOverrides,    // the entity's overrides and unsaved edits dropped: the asset's values again
    };
    // "assign" | "browse" | "clear" | "edit" | "editHere" | "createInstance" | "find" |
    // "copyPath" | "saveAsAsset" | "promote" | "clearOverrides"; "none".
    const char*       MaterialRowActionName(MaterialRowAction a);
    MaterialRowAction MaterialRowActionFromName(const std::string& name);   // None when unknown

    struct MaterialRowResult
    {
        MaterialRowAction action = MaterialRowAction::None;
        std::string       path;
        // A drag-drop activates no widget, so an undo bracket around the row
        // (DrawComponentWithUndo) records nothing for it: the handler must.
        bool              dropped = false;
    };

    // The type tag and the one-line summary of @p m (e.g. "Opaque - Default Lit";
    // the parts are joined by a middle dot, U+00B7).
    void DescribeMaterial(const MaterialComponent& m, bool instance, std::string& tag, std::string& summary);

    // One row in the current window, its widgets scoped by @p id.
    MaterialRowResult DrawMaterialRow(const char* id, const MaterialRowInfo& info);
}
