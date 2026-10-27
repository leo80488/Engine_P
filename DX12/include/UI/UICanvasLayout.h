#pragma once

// UICanvasLayout - the Canvas UI layout + paint KERNEL.
//
// ONE implementation of "UIRect tree -> resolved rects -> UIDrawList", shared by
// every consumer:
//   * UICanvasSystem, the game screen: lays out EVERY canvas at the viewport size
//     and stores the result in the UIComputedRect pool, the layout output the hit
//     test, focus navigation and Lua read.
//   * The UI Editor's designer view (UIViewService) and Game.exe -ui_render: lay
//     out ONE subject at an authored design resolution into a private
//     std::vector<LayoutNode> and never touch UIComputedRect. UIComputedRect has
//     one value per entity, so a second layout at another screen size written
//     there would overwrite the game's every frame (and thrash its fingerprint).
//
// Because both call these functions, a designer picture can disagree with the
// game only through its INPUTS (screen size, zoom), never through its code. The
// designer-only knobs (LayoutRoot for a node root, ignoreVisibility,
// PaintOptions) default to the game screen's behaviour.

#include "UI/UICanvas.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace UI
{
    // ==== Geometry ====
    Rect IntersectRects(const Rect& a, const Rect& b);
    bool HasArea(const Rect& r);

    // Unity RectTransform: parent rect + UIRect -> rect. `scale` multiplies the
    // pixel-denominated fields (size / offset) so a subtree scales with its canvas
    // scaler. Authored anchors / pivot are sanitized locally so malformed data
    // cannot invert the output.
    Rect ComputeRect(const UIRect& r, const Rect& parent, float scale);

    // The canvas scaler: 1 for ConstantPixelSize; for ScaleWithScreenSize the
    // log2 lerp of screen / reference by matchWidthOrHeight (Unity CanvasScaler).
    float ComputeCanvasScale(const UICanvas& canvas, const Vec2& screen);

    // ==== Step 1: the hierarchy ====
    // parent -> its UI children, sorted by (siblingIndex, entity): the order
    // layout, hit test and paint all use.
    struct ChildLists
    {
        std::unordered_map<Entity, std::vector<Entity>> children;
        const std::vector<Entity>* Find(Entity parent) const
        {
            auto it = children.find(parent);
            return it == children.end() ? nullptr : &it->second;
        }
    };

    // Resolve every UIParent.parent that is unset (or dead) from its parentGuid
    // through ECS::GuidRegistry (the one World write layout makes, deferred from
    // scene load / prefab spawn), then build the sorted child lists.
    // @return true while some parentGuid has no registered entity yet; the caller
    // must retry next frame (no component pool changes when it appears).
    bool BuildChildLists(World& world, ChildLists& out);

    // ==== The UI tree as the editor walks it ====
    // One rule for "who is this node's UI parent", used by layout, delete, duplicate,
    // the undo snapshot and the Outliner alike, so a subtree deleted or duplicated in
    // the frame it appeared (its edge holds only the parent's GUID yet) keeps its
    // children.

    // The parent @p up names NOW: the resolved entity while it is alive, else the
    // entity its parentGuid is registered to. NullEntity for a root or a dangling
    // edge. Never writes (BuildChildLists is the one that caches the resolve).
    Entity ResolveUIParent(const World& world, const UIParent& up);
    // The same for @p child's edge; NullEntity when it has no UIParent.
    Entity ResolveUIParent(const World& world, Entity child);
    // A Canvas UI node: a canvas root or a UIRect node.
    bool   IsUINode(const World& world, Entity e);
    // The canvas @p e belongs to (itself when it is one), up the UIParent edges;
    // NullEntity when no canvas owns it (a template root, a dangling subtree).
    Entity FindOwningCanvas(const World& world, Entity e);
    // @p parent's UI children in paint order - (siblingIndex, entity), the order
    // BuildChildLists gives layout - resolved like ResolveUIParent.
    void   UIChildrenOf(const World& world, Entity parent, std::vector<Entity>& out);
    // Would making @p newParent the UI parent of @p child form a cycle? (newParent
    // is child, or below it - up the UIParent edges.)
    bool   UIWouldCycle(const World& world, Entity child, Entity newParent);

    // ==== Step 2: one subtree ====
    struct LayoutNode
    {
        Entity         e = NullEntity;
        UIComputedRect cr;
        // Laid out only because LayoutRoot::ignoreVisibility asked: an authored
        // visible=false node (or a descendant of one). The game screen never has
        // these; a designer paints them ghosted.
        bool           hidden = false;
    };

    struct LayoutRoot
    {
        Entity root = NullEntity;
        // A canvas root's rect IS rootRect (the screen) and it is never culled.
        // A node root (a UI template under no canvas, laid out by the designer)
        // gets rootRect as its resolved rect, its own UIClipRect and its own
        // visibility, exactly like any other node.
        bool   rootIsCanvas = true;
        Rect   rootRect;
        Rect   rootClip;              // the clip the root inherits
        float  scale = 1.f;           // canvas scaler result
        int    sortBase = 0;          // canvas sortOrder * 100000
        bool   rootVisible = true;    // a canvas root: UICanvas::visible
        bool   ignoreVisibility = false;
    };

    // Lay out @p root and every node reachable from it through UIParent edges,
    // appending to @p out in DFS pre-order (= ascending sortKey). @p orderCounter
    // and @p visited are shared across the canvases of one rebuild: the counter
    // keeps equal-sortOrder canvases grouped by entity, the set stops a UIParent
    // cycle from hanging the DFS and an entity reachable from two canvases from
    // being laid out twice.
    void LayoutSubtree(World& world, const ChildLists& lists, const LayoutRoot& root,
                       int& orderCounter, std::unordered_set<Entity>& visited,
                       std::vector<LayoutNode>& out);

    // ==== Paint ====
    // Designer-only override of the state an interactable's tint transition shows.
    enum class StatePreview : uint8_t { Live = 0, Normal, Hovered, Pressed, Disabled };

    // Word-wrap cache kept OUTSIDE the component. UIText caches its wrapped string
    // keyed on the resolved pixel width; a designer at another canvas scale sharing
    // that cache would rebuild every wrapped label twice a frame (once per view).
    struct TextWrapCache
    {
        struct Entry { std::string in, out; float widthPx = -1.f, sizePx = -1.f; };
        std::unordered_map<Entity, Entry> entries;
    };

    struct PaintOptions
    {
        float          timeSec     = 0.f;               // drives text jitter
        StatePreview   state       = StatePreview::Live;
        Entity         stateTarget = NullEntity;        // NullEntity = every interactable
        float          alphaMul    = 1.f;               // ghosted hidden nodes
        TextWrapCache* wrapCache   = nullptr;           // null = UIText's own cache
    };

    // Pool pointers fetched ONCE per paint, not per drawable per component.
    struct PaintPools
    {
        ComponentPool<UIImage>*        img = nullptr;
        ComponentPool<UIText>*         txt = nullptr;
        ComponentPool<UIInteractable>* it  = nullptr;
        static PaintPools From(World& world);
    };

    // Emit one laid-out node: its UIImage (plain quad or 9-slice, clipped on the
    // CPU) and its UIText (aligned in the rect, wrapped, with effects). Glyphs bake
    // on demand here, so this runs only in the CPU paint window (UIFramePainter),
    // before FontLibrary::FlushPendingUploads.
    void PaintNode(const PaintPools& pools, Entity e, const UIComputedRect& cr,
                   UIDrawList& drawList, const PaintOptions& opt);

    // Paint every drawable node of a private layout (the designer path), in the
    // layout's order. Hidden nodes (LayoutNode::hidden) are painted at
    // @p ghostAlpha; culled nodes are skipped exactly as the game screen skips them.
    void PaintLayout(World& world, const std::vector<LayoutNode>& nodes,
                     UIDrawList& drawList, const PaintOptions& opt, float ghostAlpha = 0.35f);

} // namespace UI
