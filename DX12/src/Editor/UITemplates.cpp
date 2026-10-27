// UITemplates.cpp - see the header. The one definition of the widgets, so the main
// menu's Create > UI and the UI Editor's Palette make exactly the same subtrees.
#include "Editor/UITemplates.h"

#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "Editor/Undo/ComponentEditAction.h"   // AddComponentAction
#include "UI/UICanvasLayout.h"   // UI::IsUINode, UIChildrenOf

#include <algorithm>
#include <cctype>
#include <vector>

namespace Editor::UITemplates
{
namespace
{
    struct Def
    {
        const char* name;
        const char* label;
        const char* tooltip;
        float       w, h;
    };
    constexpr Def kDefs[static_cast<int>(Widget::Count)] = {
        { "Panel",      "Panel",           "A solid panel (Canvas Image, no texture).",                       300.f, 200.f },
        { "Image",      "Image",           "A textured quad - drop a .itex on its Texture.",                 120.f, 120.f },
        { "Text",       "Text",            "SDF text (Canvas Text).",                                         220.f,  40.f },
        { "Button",     "Button",          "Image + Interactable (tint on hover / press) + a Label child.",   200.f,  56.f },
        { "Sprite",     "Animated Sprite", "Image + Sprite Animation (a sprite sheet, 4 x 4 at 12 fps).",     128.f, 128.f },
        { "ClipPanel",  "Clip Panel",      "A panel that clips its children to its rect (UI Clip Rect).",     300.f, 200.f },
        { "EmptyRect",  "Empty Rect",      "A rect with nothing drawn - a layout group for children.",        160.f,  40.f },
    };

    const Def& D(Widget w)
    {
        const int i = std::clamp(static_cast<int>(w), 0, static_cast<int>(Widget::Count) - 1);
        return kDefs[i];
    }

    int NextSibling(World& world, Entity parent)
    {
        std::vector<Entity> kids;
        UI::UIChildrenOf(world, parent, kids);
        int next = 0;
        for (const Entity k : kids)
            if (const UI::UIRect* r = world.GetComponent<UI::UIRect>(k)) next = (std::max)(next, r->siblingIndex + 1);
        return next;
    }

    Entity Node(World& world, const char* name, Entity parent, UI::Vec2 size, int sibling)
    {
        const Entity e = world.CreateEntity();
        world.SetName(e, name);
        UI::UIRect r;
        r.size         = size;
        r.siblingIndex = sibling;
        world.AddComponent<UI::UIRect>(e, r);
        UI::AttachUIParent(world, e, parent);
        return e;
    }
}

const char* Name(Widget w)    { return D(w).name; }
const char* Label(Widget w)   { return D(w).label; }
const char* Tooltip(Widget w) { return D(w).tooltip; }
UI::Vec2    DefaultSize(Widget w) { return { D(w).w, D(w).h }; }

bool FromName(const std::string& s, Widget& out)
{
    std::string l;
    for (const char c : s)
        if (c != ' ' && c != '_') l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i < static_cast<int>(Widget::Count); ++i)
    {
        std::string n;
        for (const char* c = kDefs[i].name; *c; ++c) n += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        if (l == n || (i == static_cast<int>(Widget::Sprite) && l == "animatedsprite"))
        {
            out = static_cast<Widget>(i);
            return true;
        }
    }
    return false;
}

Entity Create(World& world, Widget w, Entity parent)
{
    if (!UI::IsUINode(world, parent)) return NullEntity;
    const Def& d = D(w);
    const Entity e = Node(world, d.name, parent, { d.w, d.h }, NextSibling(world, parent));
    switch (w)
    {
    case Widget::Panel:
    {
        UI::UIImage img;
        img.color = { 0.10f, 0.11f, 0.15f, 0.94f };
        world.AddComponent<UI::UIImage>(e, img);
        break;
    }
    case Widget::Image:
        world.AddComponent<UI::UIImage>(e, UI::UIImage{});
        break;
    case Widget::Text:
    {
        UI::UIText t;
        t.text   = "New Text";
        t.alignH = 1;
        t.alignV = 1;
        world.AddComponent<UI::UIText>(e, t);
        break;
    }
    case Widget::Button:
    {
        UI::UIImage img;
        img.color = { 0.22f, 0.46f, 0.86f, 1.f };
        world.AddComponent<UI::UIImage>(e, img);
        UI::UIInteractable it;
        it.tintTransition = true;
        world.AddComponent<UI::UIInteractable>(e, it);
        const Entity lbl = world.CreateEntity();
        world.SetName(lbl, "Label");
        UI::UIRect lr;
        lr.anchorMin = { 0.f, 0.f };
        lr.anchorMax = { 1.f, 1.f };
        lr.pivot     = { 0.5f, 0.5f };
        lr.size      = { 0.f, 0.f };
        lr.offset    = { 0.f, 0.f };
        world.AddComponent<UI::UIRect>(lbl, lr);
        UI::UIText lt;
        lt.text   = "Button";
        lt.alignH = 1;
        lt.alignV = 1;
        world.AddComponent<UI::UIText>(lbl, lt);
        UI::AttachUIParent(world, lbl, e);
        break;
    }
    case Widget::Sprite:
    {
        UI::UIImage img;            // drop a sprite-sheet .itex on its Texture
        img.pointFilter = true;     // crisp pixel-art frames by default
        world.AddComponent<UI::UIImage>(e, img);
        world.AddComponent<UI::UISpriteAnimComponent>(e, UI::UISpriteAnimComponent{});
        break;
    }
    case Widget::ClipPanel:
    {
        UI::UIImage img;
        img.color = { 0.10f, 0.11f, 0.15f, 0.94f };
        world.AddComponent<UI::UIImage>(e, img);
        world.AddComponent<UI::UIClipRect>(e, UI::UIClipRect{});
        break;
    }
    default:
        break;   // EmptyRect: the rect alone
    }
    return e;
}

void PlacePivotAt(World& world, Entity root, UI::Vec2 at, const UI::Rect& parentRect, float scale)
{
    UI::UIRect* r = world.GetComponent<UI::UIRect>(root);
    if (!r || scale <= 0.f) return;
    // ComputeRect: pivotPt = anchorLo + pivot*span + offset*scale, so the offset that
    // puts the pivot on `at` is (at - (anchorLo + pivot*span)) / scale.
    const auto c01 = [](float v) { return std::clamp(v, 0.f, 1.f); };
    float aMinX = c01(r->anchorMin.x), aMaxX = c01(r->anchorMax.x);
    float aMinY = c01(r->anchorMin.y), aMaxY = c01(r->anchorMax.y);
    if (aMinX > aMaxX) std::swap(aMinX, aMaxX);
    if (aMinY > aMaxY) std::swap(aMinY, aMaxY);
    const float pw = parentRect.mx.x - parentRect.mn.x, ph = parentRect.mx.y - parentRect.mn.y;
    const float refX = parentRect.mn.x + aMinX * pw + c01(r->pivot.x) * (aMaxX - aMinX) * pw;
    const float refY = parentRect.mn.y + aMinY * ph + c01(r->pivot.y) * (aMaxY - aMinY) * ph;
    r->offset = { (at.x - refX) / scale, (at.y - refY) / scale };
}

std::unique_ptr<Undo::IEditAction> GuidStampAction(World& world, Entity parent)
{
    const GuidComponent* gc = world.GetComponent<GuidComponent>(parent);
    if (!gc || !gc->guid.IsValid()) return nullptr;
    const ECS::Guid g = gc->guid;
    auto add = [g](World& w, Entity e)
    {
        if (w.HasComponent<GuidComponent>(e)) return;
        GuidComponent c;
        c.guid = g;
        w.AddComponent<GuidComponent>(e, c);
        ECS::GuidRegistry::Get().Register(g, e);
    };
    auto remove = [g](World& w, Entity e)
    {
        if (!w.HasComponent<GuidComponent>(e)) return;
        w.RemoveComponent<GuidComponent>(e);
        ECS::GuidRegistry::Get().Unregister(g);
    };
    return std::make_unique<Undo::AddComponentAction>(world.MakeHandle(parent), add, remove, "Guid");
}

} // namespace Editor::UITemplates
