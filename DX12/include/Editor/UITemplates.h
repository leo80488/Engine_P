#pragma once

// UITemplates - the built-in Canvas UI widgets: what the main menu's Create > UI and
// the UI Editor's Palette both make, so the two cannot drift. A widget is a small UI
// subtree - a Button is an image, an interactable and a label child - created under a
// UI parent (a canvas or a UIRect node), LAST among its siblings, centred on the
// parent unless placed.
//
// Plain World work, no ImGui, no undo: the caller brackets the spawn (RecordSpawn).

#include "ECS/ECS.h"
#include "UI/UICanvas.h"

#include <memory>
#include <string>

namespace Editor::Undo { class IEditAction; }

namespace Editor::UITemplates
{
    enum class Widget : int { Panel = 0, Image, Text, Button, Sprite, ClipPanel, EmptyRect, Count };

    const char* Name(Widget w);                          // "Panel" ... - also the entity's name
    const char* Label(Widget w);                         // the palette's label
    const char* Tooltip(Widget w);
    bool        FromName(const std::string& s, Widget& out);   // "panel", "button" ... (any case)
    UI::Vec2    DefaultSize(Widget w);                   // reference px

    // Create @p w under the UI node @p parent, last among its siblings, centred.
    // NullEntity when @p parent is no UI node.
    Entity Create(World& world, Widget w, Entity parent);

    // Move @p root (point-anchored or not) so its PIVOT lands on @p at - design px of
    // the layout @p parentRect / @p scale came from (a palette drop, Lua at = {x, y}).
    void PlacePivotAt(World& world, Entity root, UI::Vec2 at, const UI::Rect& parentRect, float scale);

    // A child names its UI parent by GUID, so attaching one stamps a GUID on a parent
    // that had none (UI::AttachUIParent). This is that stamp as an undo step: undo
    // takes the GUID off again, redo puts back the SAME one the children name. Push it
    // right after the attach, in the same macro as it. nullptr: @p parent has no GUID.
    std::unique_ptr<Undo::IEditAction> GuidStampAction(World& world, Entity parent);
}
