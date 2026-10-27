#include "UI/UICanvasLayout.h"
#include "UI/Font.h"
#include "UI/FontLibrary.h"
#include "ECS/ECS.h"
#include "ECS/GuidRegistry.h"

#include <algorithm>
#include <cmath>
#include <utility>

// The layout + paint kernel shared by UICanvasSystem and the UI Editor's designer
// (UI/UICanvasLayout.h). The designer-only seams (LayoutSubtree's node root,
// ignoreVisibility, PaintOptions) default to what UICanvasSystem passes, so they
// leave the game screen's output unchanged; ui_canvas_stress pins that output.

namespace UI
{
    namespace
    {
        // Explicit DFS stack item; hoisted to a thread_local so a rebuild does not
        // heap-allocate once per canvas.
        struct StackItem
        {
            Entity e;
            Rect   parentRect;
            Rect   inheritedClip;
            int    depth;
            bool   inheritedVisible;
        };

        Color32 ToColor32(const DirectX::XMFLOAT4& c)
        {
            return Color32(
                static_cast<uint8_t>(std::clamp(c.x, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.y, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.z, 0.f, 1.f) * 255.f),
                static_cast<uint8_t>(std::clamp(c.w, 0.f, 1.f) * 255.f));
        }

        DirectX::XMFLOAT4 MulF4(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b)
        {
            return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w };
        }

        Vec2 Clamp01(Vec2 v)
        {
            v.x = std::clamp(v.x, 0.f, 1.f);
            v.y = std::clamp(v.y, 0.f, 1.f);
            return v;
        }

        // Split the destination rect and the source UV rect into a 3x3 grid and
        // emit up to nine quads: corners at their native pixel size, edges
        // stretched along one axis, the centre stretched on both.
        //
        // Each cell is clipped on the CPU and has its UVs re-derived from how much
        // survived, the same way the single-quad path does: the draw list has one
        // clip rect per command, so a cell straddling the clip boundary would
        // otherwise sample the wrong part of the texture.
        //
        // Degenerate cases shrink rather than refuse: when the destination is
        // smaller than the borders (a 20px-tall panel with 12px corners), the
        // borders are scaled down proportionally so the corners just meet instead
        // of overlapping and drawing the art inside-out.
        void EmitNineSlice(UIDrawList& drawList, const UIImage& img,
                           const Rect& dst, const Rect& clip,
                           const UITextureRef& tex, Color32 col, uint32_t samp)
        {
            const float dw = dst.Width();
            const float dh = dst.Height();
            if (dw <= 0.f || dh <= 0.f) return;

            float bl = std::max(0.f, img.sliceBorder.x);
            float bt = std::max(0.f, img.sliceBorder.y);
            float br = std::max(0.f, img.sliceBorder.z);
            float bb = std::max(0.f, img.sliceBorder.w);

            // Proportional shrink so opposite borders can never overlap.
            if (bl + br > dw && (bl + br) > 0.f)
            {
                const float k = dw / (bl + br);
                bl *= k; br *= k;
            }
            if (bt + bb > dh && (bt + bb) > 0.f)
            {
                const float k = dh / (bt + bb);
                bt *= k; bb *= k;
            }

            // Destination and source cut lines. Source is expressed in the image's
            // own uv0..uv1 window, so a 9-slice inside an atlas sub-rect works.
            const float xs[4] = { dst.mn.x, dst.mn.x + bl, dst.mx.x - br, dst.mx.x };
            const float ys[4] = { dst.mn.y, dst.mn.y + bt, dst.mx.y - bb, dst.mx.y };

            const float uSpan = img.uv1.x - img.uv0.x;
            const float vSpan = img.uv1.y - img.uv0.y;
            const float ul = (img.sliceBorder.x / img.texWidth)  * uSpan;
            const float ut = (img.sliceBorder.y / img.texHeight) * vSpan;
            const float ur = (img.sliceBorder.z / img.texWidth)  * uSpan;
            const float ub = (img.sliceBorder.w / img.texHeight) * vSpan;
            const float us[4] = { img.uv0.x, img.uv0.x + ul, img.uv1.x - ur, img.uv1.x };
            const float vs[4] = { img.uv0.y, img.uv0.y + ut, img.uv1.y - ub, img.uv1.y };

            for (int row = 0; row < 3; ++row)
            {
                for (int cxi = 0; cxi < 3; ++cxi)
                {
                    Rect cell{ Vec2{ xs[cxi], ys[row] }, Vec2{ xs[cxi + 1], ys[row + 1] } };
                    if (!HasArea(cell)) continue;          // zero-width border column

                    const Rect vis = IntersectRects(cell, clip);
                    if (!HasArea(vis)) continue;           // fully clipped away

                    const float cw = cell.Width();
                    const float ch = cell.Height();
                    const float t0x = (vis.mn.x - cell.mn.x) / cw;
                    const float t0y = (vis.mn.y - cell.mn.y) / ch;
                    const float t1x = (vis.mx.x - cell.mn.x) / cw;
                    const float t1y = (vis.mx.y - cell.mn.y) / ch;

                    const float u0 = us[cxi],     u1 = us[cxi + 1];
                    const float v0 = vs[row],     v1 = vs[row + 1];
                    const Vec2 uvA{ u0 + (u1 - u0) * t0x, v0 + (v1 - v0) * t0y };
                    const Vec2 uvB{ u0 + (u1 - u0) * t1x, v0 + (v1 - v0) * t1y };

                    drawList.AddImage(tex, vis.mn, vis.mx, uvA, uvB, col, samp);
                }
            }
        }

        DirectX::XMFLOAT4 StateColor(const UIInteractable& it)
        {
            if (it.disabled) return it.disabledColor;
            if (it.pressed)  return it.pressedColor;
            if (it.hovered)  return it.hoverColor;
            return it.normalColor;
        }

        // The tint a transition shows: the live state, unless a designer asked to
        // preview one (for every interactable, or for the one it names).
        DirectX::XMFLOAT4 TintFor(const UIInteractable& it, Entity e, const PaintOptions& opt)
        {
            if (opt.state == StatePreview::Live
                || (opt.stateTarget != NullEntity && opt.stateTarget != e))
                return StateColor(it);
            switch (opt.state)
            {
                case StatePreview::Normal:   return it.normalColor;
                case StatePreview::Hovered:  return it.hoverColor;
                case StatePreview::Pressed:  return it.pressedColor;
                case StatePreview::Disabled: return it.disabledColor;
                default:                     return StateColor(it);
            }
        }

        TextEffect MakeTextEffect(const UIText& t)
        {
            auto toC = [](const DirectX::XMFLOAT4& c) {
                return Color32(
                    static_cast<uint8_t>(std::clamp(c.x, 0.f, 1.f) * 255.f),
                    static_cast<uint8_t>(std::clamp(c.y, 0.f, 1.f) * 255.f),
                    static_cast<uint8_t>(std::clamp(c.z, 0.f, 1.f) * 255.f),
                    static_cast<uint8_t>(std::clamp(c.w, 0.f, 1.f) * 255.f));
            };
            TextEffect fx;
            fx.outline = t.fxOutline;
            fx.glow    = t.fxGlow;
            fx.shadow  = t.fxShadow;
            fx.jitter  = t.fxJitter;
            fx.outlineColor   = toC(t.outlineColor);
            fx.outlineWidthPx = t.outlineWidth;
            fx.glowColor      = toC(t.glowColor);
            fx.glowWidthPx    = t.glowWidth;
            fx.shadowColor    = toC(t.shadowColor);
            fx.shadowOffsetPx = { t.shadowOffsetX, t.shadowOffsetY };
            fx.jitterAmpPx    = t.jitterAmplitude;
            fx.jitterFreq     = t.jitterFrequency;
            // Custom text shader: path -> global slot (deduplicated); UIPass turns
            // the slot into a dynamic PSO at draw time.
            fx.customShaderSlot = t.customShaderPath.empty()
                                ? 0u : RegisterTextShader(t.customShaderPath);
            fx.customParams[0] = t.customParam0.x; fx.customParams[1] = t.customParam0.y;
            fx.customParams[2] = t.customParam0.z; fx.customParams[3] = t.customParam0.w;
            fx.customParams[4] = t.customParam1.x; fx.customParams[5] = t.customParam1.y;
            fx.customParams[6] = t.customParam1.z; fx.customParams[7] = t.customParam1.w;
            return fx;
        }
    } // anonymous namespace

    Rect IntersectRects(const Rect& a, const Rect& b)
    {
        Rect out;
        out.mn = { std::max(a.mn.x, b.mn.x), std::max(a.mn.y, b.mn.y) };
        out.mx = { std::min(a.mx.x, b.mx.x), std::min(a.mx.y, b.mx.y) };
        if (out.mx.x < out.mn.x) out.mx.x = out.mn.x;
        if (out.mx.y < out.mn.y) out.mx.y = out.mn.y;
        return out;
    }

    bool HasArea(const Rect& r)
    {
        return r.mx.x > r.mn.x && r.mx.y > r.mn.y;
    }

    Rect ComputeRect(const UIRect& r, const Rect& parent, float scale)
    {
        Vec2 anchorMin = Clamp01(r.anchorMin);
        Vec2 anchorMax = Clamp01(r.anchorMax);
        if (anchorMin.x > anchorMax.x) std::swap(anchorMin.x, anchorMax.x);
        if (anchorMin.y > anchorMax.y) std::swap(anchorMin.y, anchorMax.y);
        const Vec2 pivot = Clamp01(r.pivot);

        const Vec2 pmin = parent.mn;
        const Vec2 psize{ parent.mx.x - parent.mn.x, parent.mx.y - parent.mn.y };

        const Vec2 anchorLo{ pmin.x + anchorMin.x * psize.x,
                             pmin.y + anchorMin.y * psize.y };
        const Vec2 anchorHi{ pmin.x + anchorMax.x * psize.x,
                             pmin.y + anchorMax.y * psize.y };
        const Vec2 span{ anchorHi.x - anchorLo.x, anchorHi.y - anchorLo.y };

        const Vec2 size{ std::max(0.f, span.x + r.size.x * scale),
                         std::max(0.f, span.y + r.size.y * scale) };
        const Vec2 anchorRef{ anchorLo.x + pivot.x * span.x,
                              anchorLo.y + pivot.y * span.y };
        const Vec2 pivotPt{ anchorRef.x + r.offset.x * scale,
                            anchorRef.y + r.offset.y * scale };

        Rect out;
        out.mn = { pivotPt.x - pivot.x * size.x, pivotPt.y - pivot.y * size.y };
        out.mx = { out.mn.x + size.x, out.mn.y + size.y };
        return out;
    }

    float ComputeCanvasScale(const UICanvas& cv, const Vec2& screenSize)
    {
        float scale = 1.f;
        if (cv.scaleMode == CanvasScaleMode::ScaleWithScreenSize)
        {
            const float refW = std::max(1.f, cv.referenceResolution.x);
            const float refH = std::max(1.f, cv.referenceResolution.y);
            const float logW = std::log2(std::max(1e-3f, screenSize.x / refW));
            const float logH = std::log2(std::max(1e-3f, screenSize.y / refH));
            const float m    = std::clamp(cv.matchWidthOrHeight, 0.f, 1.f);
            scale = std::exp2(logW * (1.f - m) + logH * m);
        }
        return scale;
    }

    bool BuildChildLists(World& world, ChildLists& out)
    {
        // clear() keeps the bucket array; the per-parent vectors' heap blocks die,
        // but the map's own allocation survives across rebuilds.
        auto& children = out.children;
        children.clear();
        bool unresolved = false;
        if (auto* parentPool = world.GetPool<UIParent>())
        {
            auto& ents = parentPool->Entities();
            auto& data = parentPool->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                UIParent& p = data[i];
                if ((p.parent == NullEntity || !world.IsAlive(p.parent)) && p.parentGuid.IsValid())
                {
                    const Entity resolved = ECS::GuidRegistry::Get().Find(p.parentGuid);
                    if (resolved != NullEntity) p.parent = resolved;
                    // Target not registered yet (deferred scene load / prefab
                    // spawn). Keep the cache open so the retry actually happens.
                    else unresolved = true;
                }
                if (p.parent != NullEntity && world.IsAlive(p.parent))
                    children[p.parent].push_back(ents[i]);
            }
        }
        // Keys extracted ONCE, then a plain pair sort: a comparator that looked
        // UIRect up would cost two GetComponent probes (type_index map + pool
        // find) per comparison, O(K log K) of them per sibling group per frame.
        // pair<int,Entity> ordering == (siblingIndex, entity) exactly.
        auto* rectPool = world.GetPool<UIRect>();
        static thread_local std::vector<std::pair<int, Entity>> t_sibKeys;
        for (auto& [parent, siblings] : children)
        {
            (void)parent;
            t_sibKeys.clear();
            t_sibKeys.reserve(siblings.size());
            for (Entity s : siblings)
            {
                const UIRect* r = rectPool ? rectPool->Get(s) : nullptr;
                t_sibKeys.emplace_back(r ? r->siblingIndex : 0, s);
            }
            std::sort(t_sibKeys.begin(), t_sibKeys.end());
            for (std::size_t k = 0; k < siblings.size(); ++k)
                siblings[k] = t_sibKeys[k].second;
        }
        return unresolved;
    }

    // ==== The UI tree as the editor walks it ====
    Entity ResolveUIParent(const World& world, const UIParent& up)
    {
        if (up.parent != NullEntity && world.IsAlive(up.parent)) return up.parent;
        if (up.parentGuid.IsValid())
        {
            const Entity p = ECS::GuidRegistry::Get().Find(up.parentGuid);
            if (p != NullEntity && world.IsAlive(p)) return p;
        }
        return NullEntity;
    }

    Entity ResolveUIParent(const World& world, Entity child)
    {
        const UIParent* up = world.GetComponent<UIParent>(child);
        return up ? ResolveUIParent(world, *up) : NullEntity;
    }

    bool IsUINode(const World& world, Entity e)
    {
        return e != NullEntity && world.IsAlive(e) &&
               (world.HasComponent<UICanvas>(e) || world.HasComponent<UIRect>(e));
    }

    Entity FindOwningCanvas(const World& world, Entity e)
    {
        for (int guard = 0; guard < 4096 && e != NullEntity && world.IsAlive(e); ++guard)
        {
            if (world.HasComponent<UICanvas>(e)) return e;
            const Entity p = ResolveUIParent(world, e);
            if (p == e) break;
            e = p;
        }
        return NullEntity;
    }

    void UIChildrenOf(const World& world, Entity parent, std::vector<Entity>& out)
    {
        out.clear();
        const auto* pool = world.GetPool<UIParent>();
        if (!pool || parent == NullEntity) return;
        const auto* rects = world.GetPool<UIRect>();
        std::vector<std::pair<int, Entity>> keyed;
        const auto& ents = pool->Entities();
        const auto& data = pool->Data();
        for (size_t i = 0; i < data.size(); ++i)
        {
            if (ents[i] == parent || !world.IsAlive(ents[i])) continue;
            if (ResolveUIParent(world, data[i]) != parent) continue;
            const UIRect* r = rects ? rects->Get(ents[i]) : nullptr;
            keyed.emplace_back(r ? r->siblingIndex : 0, ents[i]);
        }
        std::sort(keyed.begin(), keyed.end());
        out.reserve(keyed.size());
        for (const auto& [k, e] : keyed) out.push_back(e);
    }

    bool UIWouldCycle(const World& world, Entity child, Entity newParent)
    {
        Entity cur = newParent;
        for (int guard = 0; guard < 4096 && cur != NullEntity; ++guard)
        {
            if (cur == child) return true;
            const Entity p = ResolveUIParent(world, cur);
            if (p == cur) return true;   // a self-edge is a cycle already
            cur = p;
        }
        return false;
    }

    void LayoutSubtree(World& world, const ChildLists& lists, const LayoutRoot& root,
                       int& orderCounter, std::unordered_set<Entity>& visited,
                       std::vector<LayoutNode>& out)
    {
        auto* rectPool = world.GetPool<UIRect>();
        auto* clipPool = world.GetPool<UIClipRect>();
        const float scale = root.scale;

        static thread_local std::vector<StackItem> t_stack;
        t_stack.clear();
        auto pushChildren = [&](Entity parent, const Rect& pr,
                                const Rect& inheritedClip, int depth,
                                bool inheritedVisible) {
            const std::vector<Entity>* kids = lists.Find(parent);
            if (!kids) return;
            // Push reversed so first child is processed first (stable order).
            for (auto rit = kids->rbegin(); rit != kids->rend(); ++rit)
                t_stack.push_back({ *rit, pr, inheritedClip, depth, inheritedVisible });
        };

        if (root.rootIsCanvas)
        {
            // Canvas root: its rect is the screen and it is never culled; hiding
            // a canvas is UICanvas::visible, which seeds the whole subtree below.
            // Deliberately NOT inserted into `visited`: do not add it without
            // checking the game screen's pinned layout (ui_canvas_stress).
            LayoutNode n;
            n.e = root.root;
            n.cr.rect        = root.rootRect;
            n.cr.clipRect    = root.rootClip;
            n.cr.canvasScale = scale;
            n.cr.depth       = 0;
            n.cr.sortKey     = root.sortBase;
            n.cr.culled      = false;
            n.cr.valid       = true;
            out.push_back(n);
            pushChildren(root.root, root.rootRect, root.rootClip, 1, root.rootVisible);
        }
        else
        {
            // Node root (a UI template the designer lays out under no canvas): the
            // caller resolved its rect; everything else is any node's rule.
            if (!visited.insert(root.root).second) return;
            const UIRect* r = rectPool ? rectPool->Get(root.root) : nullptr;
            const bool nodeVisible = root.rootVisible && (!r || r->visible);
            Rect effectiveClip = root.rootClip;
            if (const auto* clip = clipPool ? clipPool->Get(root.root) : nullptr;
                clip && clip->enabled)
                effectiveClip = IntersectRects(effectiveClip, root.rootRect);
            LayoutNode n;
            n.e = root.root;
            n.cr.rect        = root.rootRect;
            n.cr.clipRect    = effectiveClip;
            n.cr.canvasScale = scale;
            n.cr.depth       = 0;
            n.cr.sortKey     = root.sortBase + (orderCounter++);
            n.cr.culled      = (!root.ignoreVisibility && !nodeVisible)
                            || !HasArea(IntersectRects(root.rootRect, effectiveClip));
            n.cr.valid       = true;
            n.hidden         = !nodeVisible;
            out.push_back(n);
            pushChildren(root.root, root.rootRect, effectiveClip, 1, nodeVisible);
        }

        while (!t_stack.empty())
        {
            const StackItem si = t_stack.back();
            t_stack.pop_back();

            if (!visited.insert(si.e).second) continue; // cycle / re-entry guard

            const UIRect* r = rectPool ? rectPool->Get(si.e) : nullptr;
            if (!r) continue;

            const Rect cr = ComputeRect(*r, si.parentRect, scale);
            const bool nodeVisible = si.inheritedVisible && r->visible;
            Rect effectiveClip = si.inheritedClip;
            if (const auto* clip = clipPool ? clipPool->Get(si.e) : nullptr;
                clip && clip->enabled)
                effectiveClip = IntersectRects(effectiveClip, cr);

            LayoutNode n;
            n.e = si.e;
            n.cr.rect        = cr;
            n.cr.clipRect    = effectiveClip;
            n.cr.canvasScale = scale;
            n.cr.depth       = si.depth;
            n.cr.sortKey     = root.sortBase + (orderCounter++);
            n.cr.culled      = (!root.ignoreVisibility && !nodeVisible)
                            || !HasArea(IntersectRects(cr, effectiveClip));
            n.cr.valid       = true;
            n.hidden         = !nodeVisible;
            out.push_back(n);

            pushChildren(si.e, cr, effectiveClip, si.depth + 1, nodeVisible);
        }
    }

    PaintPools PaintPools::From(World& world)
    {
        PaintPools p;
        p.img = world.GetPool<UIImage>();
        p.txt = world.GetPool<UIText>();
        p.it  = world.GetPool<UIInteractable>();
        return p;
    }

    void PaintNode(const PaintPools& pools, Entity e, const UIComputedRect& cr,
                   UIDrawList& drawList, const PaintOptions& opt)
    {
        drawList.PushClipRect(cr.clipRect);

        // Image fills the computed rect. srvGpuHandle 0 + empty path =
        // solid-colour panel (UIPass binds its 1x1 white texture). A path
        // that hasn't resolved yet is skipped to avoid a solid-colour flash.
        if (const auto* img = pools.img ? pools.img->Get(e) : nullptr; img && img->visible)
        {
            const bool waitingForTex = !img->texturePath.empty() && img->srvGpuHandle == 0;
            if (!waitingForTex)
            {
                const Rect visible = IntersectRects(cr.rect, cr.clipRect);
                const float rw = cr.rect.Width();
                const float rh = cr.rect.Height();
                if (HasArea(visible) && rw > 0.f && rh > 0.f)
                {
                    const float tx0 = (visible.mn.x - cr.rect.mn.x) / rw;
                    const float ty0 = (visible.mn.y - cr.rect.mn.y) / rh;
                    const float tx1 = (visible.mx.x - cr.rect.mn.x) / rw;
                    const float ty1 = (visible.mx.y - cr.rect.mn.y) / rh;
                    const Vec2 uv0{
                        img->uv0.x + (img->uv1.x - img->uv0.x) * tx0,
                        img->uv0.y + (img->uv1.y - img->uv0.y) * ty0
                    };
                    const Vec2 uv1{
                        img->uv0.x + (img->uv1.x - img->uv0.x) * tx1,
                        img->uv0.y + (img->uv1.y - img->uv0.y) * ty1
                    };

                    DirectX::XMFLOAT4 col = img->color;
                    if (const auto* it = pools.it ? pools.it->Get(e) : nullptr;
                        it && it->tintTransition)
                        col = MulF4(col, TintFor(*it, e, opt));
                    // Skipped at 1 so the game screen's colour is left untouched
                    // (x * 1.0f is exact anyway; this is for clarity).
                    if (opt.alphaMul != 1.f) col.w *= opt.alphaMul;

                    UITextureRef tex;
                    tex.srvGpuHandle = img->srvGpuHandle; // 0 -> white
                    const uint32_t samp =
                        UISamplerId(img->wrapMode, img->pointFilter);

                    // 9-slice, when a border is authored AND the source size
                    // is known (it is stamped by App's texture lease pass and
                    // is 0 until the texture is resident; draw the plain quad
                    // for those first frames rather than dividing by zero).
                    const bool sliced =
                        img->srvGpuHandle != 0 &&
                        img->texWidth > 0.f && img->texHeight > 0.f &&
                        (img->sliceBorder.x > 0.f || img->sliceBorder.y > 0.f ||
                         img->sliceBorder.z > 0.f || img->sliceBorder.w > 0.f);

                    if (sliced)
                    {
                        EmitNineSlice(drawList, *img, cr.rect, cr.clipRect,
                                      tex, ToColor32(col), samp);
                    }
                    else
                    {
                        drawList.AddImage(tex, visible.mn, visible.mx, uv0, uv1,
                                          ToColor32(col), samp);
                    }
                }
            }
        }

        // Text aligned within the computed rect. Each UIText resolves its
        // own font (empty path = default) and passes size/effects per call;
        // no shared font state is mutated. Missing glyphs bake on demand
        // here (CPU tick); FontLibrary::FlushPendingUploads() (App loop)
        // uploads any dirtied atlas pages before the render graph executes.
        if (auto* txt = pools.txt ? pools.txt->Get(e) : nullptr;
            txt && txt->visible && !txt->text.empty())
        {
            Font& font = FontLibrary::Get().Resolve(txt->fontPath);
            TextDrawParams tp;
            tp.sizePx  = kTextRefPx * std::max(0.f, txt->fontScale * cr.canvasScale);
            tp.effects = MakeTextEffect(*txt);
            tp.timeSec = opt.timeSec;

            // Word wrap. wrapWidth is authored in REFERENCE pixels, so it
            // scales with the canvas exactly like UIRect::size does.
            //
            // Cached and rebuilt only when the text, the resolved width or the
            // pixel size changes: a wrap is a full string rebuild, an allocation
            // per label per frame this pass must not pay. Measure / DrawText below
            // run on the wrapped copy, so alignment and clipping agree with what
            // is drawn. The game screen caches on the component; a designer passes
            // its own table (PaintOptions::wrapCache) so two views at two scales
            // do not rebuild each other's string every frame.
            const char* drawStr = txt->text.c_str();
            if (txt->wrapWidth > 0.f)
            {
                const float wpx = txt->wrapWidth * cr.canvasScale;
                if (opt.wrapCache)
                {
                    TextWrapCache::Entry& en = opt.wrapCache->entries[e];
                    if (en.in != txt->text || en.widthPx != wpx || en.sizePx != tp.sizePx)
                    {
                        en.out     = font.WrapText(txt->text.c_str(), tp.sizePx, wpx);
                        en.in      = txt->text;
                        en.widthPx = wpx;
                        en.sizePx  = tp.sizePx;
                    }
                    drawStr = en.out.c_str();
                }
                else
                {
                    if (txt->wrapCacheIn != txt->text
                            || txt->wrapCacheWidthPx != wpx
                            || txt->wrapCacheSizePx != tp.sizePx)
                    {
                        txt->wrapCacheOut     = font.WrapText(txt->text.c_str(),
                                                              tp.sizePx, wpx);
                        txt->wrapCacheIn      = txt->text;
                        txt->wrapCacheWidthPx = wpx;
                        txt->wrapCacheSizePx  = tp.sizePx;
                    }
                    drawStr = txt->wrapCacheOut.c_str();
                }
            }

            const Vec2 ts = font.Measure(drawStr, tp.sizePx);
            const float rw = cr.rect.mx.x - cr.rect.mn.x;
            const float rh = cr.rect.mx.y - cr.rect.mn.y;

            float px = cr.rect.mn.x;
            if (txt->alignH == 1)      px = cr.rect.mn.x + (rw - ts.x) * 0.5f;
            else if (txt->alignH == 2) px = cr.rect.mx.x - ts.x;

            float py = cr.rect.mn.y;
            if (txt->alignV == 1)      py = cr.rect.mn.y + (rh - ts.y) * 0.5f;
            else if (txt->alignV == 2) py = cr.rect.mx.y - ts.y;

            DirectX::XMFLOAT4 tcol = txt->color;
            if (opt.alphaMul != 1.f) tcol.w *= opt.alphaMul;
            font.DrawText(drawList, Vec2{ px, py }, ToColor32(tcol), drawStr, tp);
        }
        drawList.PopClipRect();
    }

    void PaintLayout(World& world, const std::vector<LayoutNode>& nodes,
                     UIDrawList& drawList, const PaintOptions& opt, float ghostAlpha)
    {
        const PaintPools pools = PaintPools::From(world);
        PaintOptions nodeOpt = opt;
        for (const LayoutNode& n : nodes)
        {
            if (!n.cr.valid || n.cr.culled) continue;
            const bool drawable = (pools.img && pools.img->Has(n.e))
                               || (pools.txt && pools.txt->Has(n.e));
            if (!drawable) continue;
            nodeOpt.alphaMul = n.hidden ? opt.alphaMul * ghostAlpha : opt.alphaMul;
            PaintNode(pools, n.e, n.cr, drawList, nodeOpt);
        }
    }

} // namespace UI
