#include "UI/UICanvasSystem.h"
#include "UI/UICanvas.h"
#include "UI/UICanvasLayout.h"
#include "UI/UIDocument.h"      // UpdateDocuments
#include "ECS/ECS.h"
#include "ECS/GuidRegistry.h"
#include "System/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace UI
{
    namespace
    {
        std::atomic<bool> g_wantsCaptureMouse{ false };
        std::atomic<bool> g_wantsCaptureKeyboard{ false };
        // Sticky across frames; see SetKeyboardCaptureRequest.
        std::atomic<bool> g_keyboardCaptureRequest{ false };

        // ---- Focus, process-wide ----
        // Plain, not atomic: the single writer and every reader are on the main thread
        // (UpdateInput from App::Run, Lua from the script phase). The capture flags above are
        // atomic only because Window's WndProc predicates read them.
        Entity g_focused = NullEntity;
        int    g_navX    = 0;
        int    g_navY    = 0;
        bool   g_submit  = false;
        bool   g_cancel  = false;

        Vec2 Clamp01(Vec2 v)
        {
            v.x = std::clamp(v.x, 0.f, 1.f);
            v.y = std::clamp(v.y, 0.f, 1.f);
            return v;
        }

        // ---- Structural layout fingerprint ----
        // Mixes every input the layout pass reads into one 64-bit value so an unchanged frame can
        // reuse the previous UIComputedRects. It is order-sensitive (ComponentPool::Erase
        // reshuffles the dense array), so it may report a change that lays out identically - one
        // wasted rebuild - but it must never MISS one. Hence the mechanical rule: every authored
        // field of every component the layout touches, minus the fields this system writes
        // (UICanvas::computedScale, UIInteractable::hovered/pressed/clicked; folding those in would
        // re-key the cache on every hover). Extend this function with every new layout input.
        struct LayoutFingerprint
        {
            uint64_t h = 0xcbf29ce484222325ull;          // FNV-1a 64 basis
            void W(uint32_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; }
            void W64(uint64_t v) { W(static_cast<uint32_t>(v)); W(static_cast<uint32_t>(v >> 32)); }
            void I(int v)  { W(static_cast<uint32_t>(v)); }
            void B(bool v) { W(v ? 1u : 0u); }
            void F(float v)
            {
                uint32_t bits;
                std::memcpy(&bits, &v, sizeof(bits));
                if (v == 0.f) bits = 0u;                 // -0.0f lays out as +0.0f
                W(bits);
            }
            void V2(const Vec2& v) { F(v.x); F(v.y); }
        };

        uint64_t ComputeLayoutFingerprint(World& world, const Vec2& screenSize,
                                          std::size_t computedRectCount)
        {
            LayoutFingerprint fp;
            fp.V2(screenSize);
            // Folding the output pool's size in keeps a pool losing its last component from hashing
            // like an absent pool, and makes a UIComputedRect erased behind our back force the DFS to
            // re-add it. Costs one extra rebuild the frame after new widgets appear, then settles.
            fp.W64(static_cast<uint64_t>(computedRectCount));

            if (auto* pool = world.GetPool<UICanvas>())
            {
                auto& ents = pool->Entities();
                auto& data = pool->Data();
                fp.W64(static_cast<uint64_t>(data.size()));
                for (std::size_t i = 0; i < data.size(); ++i)
                {
                    const UICanvas& c = data[i];
                    fp.W(ents[i]);
                    fp.I(static_cast<int>(c.renderMode));
                    fp.I(static_cast<int>(c.scaleMode));
                    fp.V2(c.referenceResolution);
                    fp.F(c.matchWidthOrHeight);
                    fp.I(c.sortOrder);
                    // Layout input: it gates the whole subtree, so hiding a canvas must force a rebuild.
                    fp.I(c.visible ? 1 : 0);
                }
            }
            else fp.W(0xFFFFFFFFu);

            if (auto* pool = world.GetPool<UIRect>())
            {
                auto& ents = pool->Entities();
                auto& data = pool->Data();
                fp.W64(static_cast<uint64_t>(data.size()));
                for (std::size_t i = 0; i < data.size(); ++i)
                {
                    const UIRect& r = data[i];
                    fp.W(ents[i]);
                    fp.V2(r.anchorMin); fp.V2(r.anchorMax); fp.V2(r.pivot);
                    fp.V2(r.size);      fp.V2(r.offset);
                    fp.B(r.visible);
                    fp.I(r.siblingIndex);
                }
            }
            else fp.W(0xFFFFFFFEu);

            if (auto* pool = world.GetPool<UIParent>())
            {
                auto& ents = pool->Entities();
                auto& data = pool->Data();
                fp.W64(static_cast<uint64_t>(data.size()));
                for (std::size_t i = 0; i < data.size(); ++i)
                {
                    const UIParent& p = data[i];
                    fp.W(ents[i]);
                    // `parent` is also written by GUID resolution: one extra rebuild the frame after a scene
                    // load resolves an edge, then it settles.
                    fp.W(p.parent);
                    fp.W64(p.parentGuid.hi);
                    fp.W64(p.parentGuid.lo);
                }
            }
            else fp.W(0xFFFFFFFDu);

            if (auto* pool = world.GetPool<UIClipRect>())
            {
                auto& ents = pool->Entities();
                auto& data = pool->Data();
                fp.W64(static_cast<uint64_t>(data.size()));
                for (std::size_t i = 0; i < data.size(); ++i)
                {
                    fp.W(ents[i]);
                    fp.B(data[i].enabled);
                }
            }
            else fp.W(0xFFFFFFFCu);

            // Only the two authored flags: raycastTarget decides membership of the cached target list;
            // disabled is re-read live by the hit test but folded to keep the authored-fields rule.
            if (auto* pool = world.GetPool<UIInteractable>())
            {
                auto& ents = pool->Entities();
                auto& data = pool->Data();
                fp.W64(static_cast<uint64_t>(data.size()));
                for (std::size_t i = 0; i < data.size(); ++i)
                {
                    fp.W(ents[i]);
                    fp.B(data[i].raycastTarget);
                    fp.B(data[i].disabled);
                }
            }
            else fp.W(0xFFFFFFFBu);

            // A canvas the game screen skips (the UI Editor's template stage) is a layout input:
            // adding or removing the tag moves a whole subtree in or out of the layout.
            if (auto* pool = world.GetPool<UICanvasOffscreen>())
            {
                auto& ents = pool->Entities();
                auto& data = pool->Data();
                fp.W64(static_cast<uint64_t>(data.size()));
                for (std::size_t i = 0; i < data.size(); ++i)
                {
                    fp.W(ents[i]);
                    fp.W(data[i].owner);
                }
            }
            else fp.W(0xFFFFFFFAu);

            return fp.h;
        }

        // The canvases the game screen lays out, by (sortOrder, entity). The UI Editor's template
        // stage (UICanvasOffscreen) is its designer's, never the game screen's.
        struct CanvasEntry { Entity e; UICanvas* c; };
        void CollectScreenCanvases(World& world, std::vector<CanvasEntry>& out)
        {
            out.clear();
            auto* canvasPool = world.GetPool<UICanvas>();
            if (!canvasPool) return;
            auto& ents = canvasPool->Entities();
            auto& data = canvasPool->Data();
            auto* offscreen = world.GetPool<UICanvasOffscreen>();
            out.reserve(data.size());
            for (size_t i = 0; i < data.size(); ++i)
            {
                if (offscreen && offscreen->Has(ents[i])) continue;
                out.push_back({ ents[i], &data[i] });
            }
            std::sort(out.begin(), out.end(),
                [](const CanvasEntry& a, const CanvasEntry& b) {
                    if (a.c->sortOrder != b.c->sortOrder)
                        return a.c->sortOrder < b.c->sortOrder;
                    return a.e < b.e;
                });
        }
    } // anonymous namespace

    bool UICanvasSystem::GlobalWantsCaptureMouse()
    {
        return g_wantsCaptureMouse.load(std::memory_order_relaxed);
    }

    bool UICanvasSystem::GlobalWantsCaptureKeyboard()
    {
        return g_wantsCaptureKeyboard.load(std::memory_order_relaxed);
    }

    void UICanvasSystem::SetGlobalCapture(bool wantsMouse, bool wantsKeyboard)
    {
        g_wantsCaptureMouse.store(wantsMouse, std::memory_order_relaxed);
        g_wantsCaptureKeyboard.store(wantsKeyboard, std::memory_order_relaxed);
    }

    void UICanvasSystem::MergeExternalCapture(bool wantsMouse, bool wantsKeyboard)
    {
        if (wantsMouse)    g_wantsCaptureMouse.store(true, std::memory_order_relaxed);
        if (wantsKeyboard) g_wantsCaptureKeyboard.store(true, std::memory_order_relaxed);
    }

    void UICanvasSystem::SetKeyboardCaptureRequest(bool wants)
    {
        g_keyboardCaptureRequest.store(wants, std::memory_order_relaxed);
        // Publish immediately too: a rebind screen usually sets this from a click handler, after
        // this frame's UpdateInput already published, and the player's first key would otherwise
        // still reach gameplay.
        if (wants) g_wantsCaptureKeyboard.store(true, std::memory_order_relaxed);
    }

    bool UICanvasSystem::KeyboardCaptureRequested()
    {
        return g_keyboardCaptureRequest.load(std::memory_order_relaxed);
    }

    Entity UICanvasSystem::FocusedEntityGlobal() { return g_focused; }
    int    UICanvasSystem::NavXGlobal()          { return g_navX;    }
    int    UICanvasSystem::NavYGlobal()          { return g_navY;    }
    bool   UICanvasSystem::SubmitGlobal()        { return g_submit;  }
    bool   UICanvasSystem::CancelGlobal()        { return g_cancel;  }

    bool UICanvasSystem::SetFocusGlobal(World& world, Entity e)
    {
        if (e == NullEntity) { g_focused = NullEntity; return true; }
        auto* interactionPool = world.GetPool<UIInteractable>();
        auto* crPool          = world.GetPool<UIComputedRect>();
        if (!interactionPool || !crPool) return false;
        const auto* it = interactionPool->Get(e);
        const auto* cr = crPool->Get(e);
        // `valid` as well as `culled`: a node created this frame has no layout yet, and focusing it
        // would put the highlight at (0,0) until the next frame.
        if (!it || !cr || !cr->valid || cr->culled
                || it->disabled || !it->focusable) return false;
        g_focused = e;
        return true;
    }

    // Screen-less overload; see the header. The resolve itself (steps 0-2, no interaction) is
    // the overload below.
    bool UICanvasSystem::ResolveLayout(World& world)
    {
        if (!m_haveLastScreen) return false;
        return ResolveLayout(world, m_lastScreen);
    }

    bool UICanvasSystem::ResolveLayout(World& world, const UIScreen& screen)
    {
        ZoneScopedN("UICanvas.LayoutResolve");
        m_lastScreen     = screen;
        m_haveLastScreen = true;

        // Dropping the cache also invalidates the rects it vouched for: deleting the last Canvas
        // must not leave a stale clickable rect or a latched WasUIClicked result.
        auto dropLayoutCache = [&]()
        {
            m_layoutCacheValid          = false;
            m_layoutHasUnresolvedParent = false;
            m_layoutWorld               = nullptr;
            m_raycastTargets.clear();
            // Focus rides with the rects: across a world swap a stale focus is an entity id that may
            // have been recycled, and the pad would drive whatever inherited the slot.
            g_focused = NullEntity;
            if (auto* pool = world.GetPool<UIComputedRect>())
                for (auto& cr : pool->Data()) cr.valid = false;
        };

        auto* canvasPool = world.GetPool<UICanvas>();
        if (!canvasPool || canvasPool->Size() == 0)
        {
            dropLayoutCache();
            return false;
        }

        const Vec2 screenSize = screen.size;
        if (screenSize.x <= 0.f || screenSize.y <= 0.f)
        {
            dropLayoutCache();
            return false;
        }

        auto* crPool = world.EnsurePool<UIComputedRect>();

        // ---- 0. Structural cache gate ----
        // World identity + Epoch() cover what the component sweep cannot see: another World reusing
        // this system, and Clear()/AdoptState replacing the pools with different data that sweeps
        // the same.
        const uint32_t worldEpoch  = world.Epoch();
        const uint64_t fingerprint =
            ComputeLayoutFingerprint(world, screenSize, crPool->Size());
        const bool layoutUnchanged = m_layoutCacheValid
                                  && !m_layoutHasUnresolvedParent
                                  && m_layoutWorld       == &world
                                  && m_layoutWorldEpoch  == worldEpoch
                                  && m_layoutFingerprint == fingerprint;

        if (!layoutUnchanged)
        {
            RebuildLayout(world, screenSize);
            m_layoutFingerprint = fingerprint;
            m_layoutWorldEpoch  = worldEpoch;
            m_layoutWorld       = &world;
            m_layoutCacheValid  = true;
            // Per frame, not per call: UpdateInput folds this into the streak and clears it (see
            // m_layoutRebuiltThisFrame).
            m_layoutRebuiltThisFrame = true;
        }
        else if (!m_loggedCacheActive)
        {
            m_loggedCacheActive = true;
            LOG_INFO("UICanvas: layout cache active (%zu computed rects, %zu raycast targets).",
                     crPool->Size(), m_raycastTargets.size());
        }
        return true;
    }

    void UICanvasSystem::UpdateInput(World& world, const UIInputState& input,
                                     const UIScreen& screen)
    {
        ZoneScopedN("UICanvas.Input");

        // Streak accounting lives here: this is the one entry point called exactly once a frame.
        if (m_layoutRebuiltThisFrame)
        {
            // 1800 frames (~30 s at 60 Hz) of back-to-back rebuilds is not a busy UI but an input that
            // never settles (a widget animating its own UIRect, or a field this system writes leaking
            // into the fingerprint).
            if (++m_layoutRebuildStreak >= 1800 && !m_loggedCacheThrash)
            {
                m_loggedCacheThrash = true;
                LOG_WARNING("UICanvas: layout fingerprint has not settled in %u frames — "
                            "some layout input changes every frame, so the structural "
                            "cache is pure overhead here.", m_layoutRebuildStreak);
            }
        }
        else
        {
            m_layoutRebuildStreak = 0;
        }
        m_layoutRebuiltThisFrame = false;

        // The scene's UI Document instances (a .iui it references) are spawned / respawned here,
        // once a frame in both exes, so the layout below, this frame's input and the scripts after
        // it all see them.
        UpdateDocuments(world);

        m_wantsCaptureMouse = false;
        // The keyboard half carries the sticky request forward; only the pointer half is
        // re-derived every frame. Publish once a frame, here and not in ResolveLayout: App reads it
        // to build the frame's single gameplay-input snapshot.
        SetGlobalCapture(false, KeyboardCaptureRequested());

        // `clicked` is a one-frame edge unrelated to layout: cleared whether or not the layout is
        // rebuilt.
        auto* interactionPool = world.GetPool<UIInteractable>();
        if (interactionPool)
            for (auto& it : interactionPool->Data()) it.clicked = false;

        if (!ResolveLayout(world, screen))
        {
            // No usable layout: nothing can be hovered or held, and a latched
            // press must not survive into the next frame.
            if (interactionPool)
                for (auto& it : interactionPool->Data())
                {
                    it.hovered = false;
                    it.pressed = false;
                    it.clicked = false;
                }
            return;
        }

        // ---- 3. Interaction ----
        // Runs every frame on the cached layout: the pointer moves without the hierarchy moving,
        // and hover/press are not layout inputs.
        RunInteraction(world, input);
    }

    // Steps 1 + 2: resolve parent GUIDs, build child lists, lay out every canvas subtree into
    // UIComputedRect and collect the sorted raycast list. The layout is the shared kernel
    // (UI/UICanvasLayout.h) the UI Editor's designer also runs; the game screen keeps the
    // canvas order, the UIComputedRect pool and the raycast list.
    void UICanvasSystem::RebuildLayout(World& world, const Vec2& screenSize)
    {
        ZoneScopedN("UICanvas.LayoutRebuild");

        auto* canvasPool      = world.GetPool<UICanvas>();
        auto* interactionPool = world.GetPool<UIInteractable>();
        if (!canvasPool) return;                 // caller already early-outs

        for (auto& cr : world.EnsurePool<UIComputedRect>()->Data()) cr.valid = false;

        // ---- 1. Resolve parent GUIDs + build child lists ----
        // thread_local only for lifetime: the map's bucket array survives the clear inside.
        static thread_local ChildLists t_lists;
        m_layoutHasUnresolvedParent = BuildChildLists(world, t_lists);

        // ---- 2. Lay out each canvas subtree ----
        // A template stage's canvas has no rect: not painted, not hit-tested.
        static thread_local std::vector<CanvasEntry> t_canvases;
        CollectScreenCanvases(world, t_canvases);

        m_raycastTargets.clear();

        auto* crPool = world.EnsurePool<UIComputedRect>();
        // Guards against malformed UIParent cycles (would otherwise hang the DFS)
        // and double-processing an entity reachable from two canvases.
        static thread_local std::unordered_set<Entity> t_visited;
        t_visited.clear();
        // One increasing minor order across the sorted canvas list keeps equal-sortOrder canvases
        // grouped by the Entity tie-break.
        int orderCounter = 1;
        static thread_local std::vector<LayoutNode> t_nodes;

        for (auto& ce : t_canvases)
        {
            UICanvas& cv = *ce.c;
            const float scale = ComputeCanvasScale(cv, screenSize);
            cv.computedScale = scale;

            // Canvas root rect = full screen; rootVisible from the canvas flag is what makes
            // UICanvas::visible reach the whole subtree.
            LayoutRoot root;
            root.root         = ce.e;
            root.rootIsCanvas = true;
            root.rootRect     = Rect{ Vec2{ 0.f, 0.f }, screenSize };
            root.rootClip     = root.rootRect;
            root.scale        = scale;
            root.sortBase     = cv.sortOrder * 100000;
            root.rootVisible  = cv.visible;

            t_nodes.clear();
            LayoutSubtree(world, t_lists, root, orderCounter, t_visited, t_nodes);
            for (std::size_t k = 0; k < t_nodes.size(); ++k)
            {
                const LayoutNode& n = t_nodes[k];
                crPool->Add(n.e, n.cr);
                // nodes[0] is the canvas root, never a raycast target.
                if (k == 0 || n.cr.culled) continue;
                if (const auto* it = interactionPool ? interactionPool->Get(n.e) : nullptr;
                    it && it->raycastTarget)
                    m_raycastTargets.push_back(n.e);
            }
        }

        // Front-to-back order for the hit test. Depends only on sortKey, so it
        // belongs to the rebuild and survives every cache hit with the rects.
        {
            static thread_local std::vector<std::pair<int, Entity>> t_rcKeys;
            t_rcKeys.clear();
            t_rcKeys.reserve(m_raycastTargets.size());
            for (Entity e : m_raycastTargets)
            {
                const auto* c = crPool->Get(e);
                // Negated key: ascending pair sort == descending sortKey.
                t_rcKeys.emplace_back(c ? -c->sortKey : 0, e);
            }
            std::sort(t_rcKeys.begin(), t_rcKeys.end());
            for (std::size_t k = 0; k < m_raycastTargets.size(); ++k)
                m_raycastTargets[k] = t_rcKeys[k].second;
        }
    }

    // Step 3: hit-test the pointer against the cached raycast list and advance
    // hover/press/click/focus. Never touches layout, so it runs on cache hits too.
    void UICanvasSystem::RunInteraction(World& world, const UIInputState& input)
    {
        ZoneScopedN("UICanvas.Interaction");

        auto* crPool          = world.GetPool<UIComputedRect>();
        auto* interactionPool = world.GetPool<UIInteractable>();
        if (!crPool) return;

        const Vec2 mouse = input.mousePos;
        Entity hovered = NullEntity;
        for (Entity e : m_raycastTargets)
        {
            const auto* cr = crPool->Get(e);
            const auto* it = interactionPool ? interactionPool->Get(e) : nullptr;
            if (!cr || !it || it->disabled) continue;
            if (cr->rect.Contains(mouse) && cr->clipRect.Contains(mouse))
            {
                hovered = e;
                break;
            }
        }
        bool pointerOwned = (hovered != NullEntity);

        const bool mouseHeld = input.mouseLeft;
        const bool mouseDown = input.mouseDown;
        const bool mouseUp   = input.mouseUp;
        static thread_local std::unordered_set<Entity> t_activeTargets;
        t_activeTargets.clear();
        t_activeTargets.insert(m_raycastTargets.begin(), m_raycastTargets.end());

        if (interactionPool)
        {
            auto& ents = interactionPool->Entities();
            auto& data = interactionPool->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                UIInteractable& it = data[i];
                const Entity e = ents[i];
                if (it.disabled || !t_activeTargets.contains(e))
                {
                    it.hovered = false;
                    it.pressed = false;
                    continue;
                }
                const bool ownedBeforeUpdate = it.pressed;
                it.hovered = (e == hovered);
                if (mouseDown && it.hovered) it.pressed = true;
                if (mouseUp)
                {
                    if (it.pressed && it.hovered) it.clicked = true; // release inside
                    it.pressed = false;
                }
                else if (!mouseHeld)
                {
                    it.pressed = false;
                }
                // Once a target owns a press, keep UI capture while dragging outside its rect and through
                // the release frame, or world input takes over the same gesture.
                pointerOwned = pointerOwned || it.pressed
                             || (mouseUp && ownedBeforeUpdate);
            }
        }
        // ---- Focus + directional navigation ----
        // After the pointer pass on purpose, so hover is resolved and can claim focus: a mouse and
        // a pad must never disagree about what is selected.
        g_navX   = input.navX;
        g_navY   = input.navY;
        g_submit = input.submit;
        g_cancel = input.cancel;

        if (interactionPool)
        {
            // Pointer hover wins the focus outright - only on a real hover, or moving the mouse off
            // the menu would drop a selection the pad just set.
            if (hovered != NullEntity)
            {
                const auto* hit = interactionPool->Get(hovered);
                if (hit && hit->focusable && !hit->disabled) g_focused = hovered;
            }

            // Drop a focus that is no longer a legal target (page switched, row disabled, entity
            // destroyed), or the pad keeps driving an element that is not on screen.
            if (g_focused != NullEntity)
            {
                const auto* f  = interactionPool->Get(g_focused);
                const auto* fc = crPool->Get(g_focused);
                if (!f || !fc || f->disabled || !f->focusable || fc->culled
                        || !t_activeTargets.contains(g_focused))
                    g_focused = NullEntity;
            }

            // Nothing focused but a navigation press arrived: adopt the first legal candidate so the
            // first d-pad press does something visible.
            const bool navPressed = (g_navX != 0 || g_navY != 0);
            if (g_focused == NullEntity && navPressed)
            {
                for (Entity e : m_raycastTargets)
                {
                    const auto* it = interactionPool->Get(e);
                    const auto* cr = crPool->Get(e);
                    if (it && cr && it->focusable && !it->disabled && !cr->culled)
                    {
                        g_focused = e;
                        break;
                    }
                }
            }
            else if (navPressed)
            {
                // The focused element may own the X axis (slider, cycler): consume only the axis it does
                // not claim.
                const auto* f = interactionPool->Get(g_focused);
                const int dx = (f && f->absorbNavX) ? 0 : g_navX;
                if (dx != 0 || g_navY != 0) NavigateFocus(world, dx, g_navY);
            }

            // Submit sets the same one-frame `clicked` edge a mouse release sets, so every consumer
            // (Engine.WasUIClicked, gameplay polling) works with a pad with no second code path.
            if (input.submit && g_focused != NullEntity)
            {
                if (auto* f = interactionPool->Get(g_focused))
                    if (!f->disabled) f->clicked = true;
            }

            // Publish the flag last, once, so exactly one element carries it.
            auto& fents = interactionPool->Entities();
            auto& fdata = interactionPool->Data();
            for (size_t i = 0; i < fdata.size(); ++i)
                fdata[i].focused = (fents[i] == g_focused);
        }

        m_wantsCaptureMouse = pointerOwned;
        SetGlobalCapture(m_wantsCaptureMouse, KeyboardCaptureRequested());
    }

    bool UICanvasSystem::NavigateFocus(World& world, int dx, int dy)
    {
        if (dx == 0 && dy == 0) return false;
        auto* interactionPool = world.GetPool<UIInteractable>();
        auto* crPool          = world.GetPool<UIComputedRect>();
        if (!interactionPool || !crPool) return false;
        const auto* fromCr = crPool->Get(g_focused);
        if (!fromCr) return false;

        const Vec2 from{ (fromCr->rect.mn.x + fromCr->rect.mx.x) * 0.5f,
                         (fromCr->rect.mn.y + fromCr->rect.mx.y) * 0.5f };

        Entity best = NullEntity;
        float  bestScore = 0.f;
        for (Entity e : m_raycastTargets)
        {
            if (e == g_focused) continue;
            const auto* it = interactionPool->Get(e);
            const auto* cr = crPool->Get(e);
            if (!it || !cr || it->disabled || !it->focusable || cr->culled) continue;

            const Vec2 to{ (cr->rect.mn.x + cr->rect.mx.x) * 0.5f,
                           (cr->rect.mn.y + cr->rect.mx.y) * 0.5f };
            const float ox = to.x - from.x;
            const float oy = to.y - from.y;

            // Must lie in the requested direction along the primary axis. The 0.5 px epsilon rejects
            // exact ties, which would let a same-row neighbour answer a vertical press.
            const float primary = (dy != 0) ? oy * static_cast<float>(dy)
                                            : ox * static_cast<float>(dx);
            if (primary <= 0.5f) continue;

            // Off-axis distance weighs 4x so navigation prefers the element straight ahead over a
            // nearer one off to the side: a two-column layout navigates as two columns.
            const float offAxis = (dy != 0) ? std::fabs(ox) : std::fabs(oy);
            const float score   = primary + offAxis * 4.0f;
            if (best == NullEntity || score < bestScore)
            {
                best      = e;
                bestScore = score;
            }
        }
        if (best == NullEntity) return false;   // edge of the list: stay put
        g_focused = best;
        return true;
    }

    void UICanvasSystem::Paint(World& world, UIDrawList& drawList, float dt)
    {
        ZoneScopedN("UICanvas.Paint");
        if (dt > 0.f) m_uiTimeSec += dt;

        PaintDrawables(world, drawList);

        // The Canvas part's totals, before the diagnostic wireframe. The hash walks every vertex,
        // so it is computed only when asked for.
        m_lastPaint.verts   = static_cast<uint32_t>(drawList.Vertices().size());
        m_lastPaint.indices = static_cast<uint32_t>(drawList.Indices().size());
        m_lastPaint.cmds    = static_cast<uint32_t>(drawList.Commands().size());
        m_lastPaint.effects = static_cast<uint32_t>(drawList.Effects().size());
        if (m_paintHash) m_lastPaint.hash = drawList.ContentHash();
        ++m_lastPaint.paints;

        PaintDebugWireframe(world, drawList);
    }

    void UICanvasSystem::PaintDrawables(World& world, UIDrawList& drawList)
    {
        // Pool pointers once, so the loop does not probe the type_index map per drawable per
        // component (ECS.h forbids that in per-frame systems).
        auto* crPool = world.GetPool<UIComputedRect>();
        const PaintPools pools = PaintPools::From(world);

        // ---- 4. Emit drawables (back-to-front) ----
        struct Drawable { Entity e; int sortKey; };
        static thread_local std::vector<Drawable> t_drawables;
        t_drawables.clear();
        if (crPool)
        {
            const auto& entities = crPool->Entities();
            const auto& rects = crPool->Data();
            t_drawables.reserve(rects.size());
            for (size_t i = 0; i < rects.size(); ++i)
            {
                if (!rects[i].valid || rects[i].culled) continue;
                const Entity e = entities[i];
                if ((pools.img && pools.img->Has(e)) || (pools.txt && pools.txt->Has(e)))
                    t_drawables.push_back({ e, rects[i].sortKey });
            }
        }
        std::sort(t_drawables.begin(), t_drawables.end(),
            [](const Drawable& a, const Drawable& b) { return a.sortKey < b.sortKey; });

        PaintOptions opt;
        opt.timeSec = m_uiTimeSec;
        for (const auto& d : t_drawables)
        {
            const UIComputedRect* cr = crPool ? crPool->Get(d.e) : nullptr;
            if (!cr || !cr->valid) continue;
            PaintNode(pools, d.e, *cr, drawList, opt);
        }
    }

    void UICanvasSystem::LayoutScreen(World& world, const Vec2& screenSize, std::vector<LayoutNode>& out) const
    {
        ZoneScopedN("UICanvas.LayoutScreen");
        out.clear();
        if (screenSize.x <= 0.f || screenSize.y <= 0.f) return;
        static thread_local ChildLists t_lists;
        BuildChildLists(world, t_lists);
        static thread_local std::vector<CanvasEntry> t_canvases;
        CollectScreenCanvases(world, t_canvases);
        static thread_local std::unordered_set<Entity> t_visited;
        t_visited.clear();
        int orderCounter = 1;
        for (const CanvasEntry& ce : t_canvases)
        {
            const UICanvas& cv = *ce.c;
            LayoutRoot root;
            root.root         = ce.e;
            root.rootIsCanvas = true;
            root.rootRect     = Rect{ Vec2{ 0.f, 0.f }, screenSize };
            root.rootClip     = root.rootRect;
            root.scale        = ComputeCanvasScale(cv, screenSize);
            root.sortBase     = cv.sortOrder * 100000;
            root.rootVisible  = cv.visible;
            LayoutSubtree(world, t_lists, root, orderCounter, t_visited, out);
        }
    }

    void UICanvasSystem::PaintScreen(World& world, const std::vector<LayoutNode>& nodes, UIDrawList& drawList,
                                     TextWrapCache& wrap) const
    {
        ZoneScopedN("UICanvas.PaintScreen");
        PaintOptions opt;
        opt.timeSec   = m_uiTimeSec;
        opt.wrapCache = &wrap;
        PaintLayout(world, nodes, drawList, opt);
    }

    void UICanvasSystem::PaintDebugWireframe(World& world, UIDrawList& drawList)
    {
        if (!m_debugWireframe) return;

        auto* crPool = world.GetPool<UIComputedRect>();
        if (!crPool) return;

        struct DebugEntry
        {
            Entity e;
            int    sortKey;
        };

        std::vector<DebugEntry> entries;
        const auto& entities = crPool->Entities();
        const auto& rects    = crPool->Data();
        entries.reserve(rects.size());
        for (size_t i = 0; i < rects.size(); ++i)
        {
            if (rects[i].valid)
                entries.push_back({ entities[i], rects[i].sortKey });
        }

        // Preserve Canvas/tree order for overlapping outlines, but always draw
        // the selected node last so it remains identifiable.
        std::sort(entries.begin(), entries.end(), [&](const DebugEntry& a,
                                                       const DebugEntry& b) {
            const bool aSelected = a.e == m_debugSelectedEntity;
            const bool bSelected = b.e == m_debugSelectedEntity;
            if (aSelected != bSelected) return !aSelected;
            if (a.sortKey != b.sortKey) return a.sortKey < b.sortKey;
            return a.e < b.e;
        });

        constexpr size_t kMaxIndices  = 65535u; // UIPass / uint16 draw-list limit
        constexpr size_t kMaxVertices = 32768u; // UIPass source-vertex budget
        constexpr size_t kSelectedReserveIndices  = 72u;
        constexpr size_t kSelectedReserveVertices = 48u;
        constexpr float  kCollapsedMarkerRadius = 4.f;

        const Color32 layoutColor   {  40, 210, 255, 220 }; // cyan
        const Color32 interactColor {  70, 255, 130, 230 }; // green
        const Color32 canvasColor   { 205, 105, 255, 230 }; // purple
        const Color32 culledColor   { 255,  70,  70, 220 }; // red
        const Color32 clipColor     { 255, 165,  40, 235 }; // orange
        const Color32 selectedColor { 255, 235,  45, 255 }; // yellow

        auto finiteRect = [](const Rect& r) {
            return std::isfinite(r.mn.x) && std::isfinite(r.mn.y)
                && std::isfinite(r.mx.x) && std::isfinite(r.mx.y);
        };
        auto sameRect = [](const Rect& a, const Rect& b) {
            constexpr float eps = 0.25f;
            return std::abs(a.mn.x - b.mn.x) <= eps
                && std::abs(a.mn.y - b.mn.y) <= eps
                && std::abs(a.mx.x - b.mx.x) <= eps
                && std::abs(a.mx.y - b.mx.y) <= eps;
        };
        auto hasRoomForLines = [&](size_t lineCount, bool selected) {
            const size_t indexReserve  = selected ? 0u : kSelectedReserveIndices;
            const size_t vertexReserve = selected ? 0u : kSelectedReserveVertices;
            return drawList.Indices().size() + lineCount * 6u + indexReserve <= kMaxIndices
                && drawList.Vertices().size() + lineCount * 4u + vertexReserve <= kMaxVertices;
        };
        auto addCross = [&](const Vec2& p, Color32 color, float radius,
                            float thickness, bool selected) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)
                || !hasRoomForLines(2u, selected))
                return;
            drawList.AddLine({ p.x - radius, p.y }, { p.x + radius, p.y },
                             color, thickness);
            drawList.AddLine({ p.x, p.y - radius }, { p.x, p.y + radius },
                             color, thickness);
        };
        auto addOutline = [&](const Rect& r, Color32 color, float thickness,
                              bool selected) {
            if (!finiteRect(r)) return;
            if (r.Width() > 1e-4f && r.Height() > 1e-4f)
            {
                if (hasRoomForLines(4u, selected))
                    drawList.AddRect(r.mn, r.mx, color, thickness);
                return;
            }

            // A zero-width/height rect still matters when diagnosing a bad anchor/size combination:
            // mark it with a cross instead of four degenerate lines.
            const Vec2 center{ (r.mn.x + r.mx.x) * 0.5f,
                               (r.mn.y + r.mx.y) * 0.5f };
            addCross(center, color, kCollapsedMarkerRadius, thickness, selected);
        };

        // Outline primitives must sample the default white texture even when a
        // custom Canvas image was the last regular draw command.
        drawList.PushTexture(UITextureRef{});
        for (const DebugEntry& entry : entries)
        {
            const UIComputedRect* cr = world.GetComponent<UIComputedRect>(entry.e);
            if (!cr || !cr->valid) continue;
            const bool selected = entry.e == m_debugSelectedEntity;

            Color32 semanticColor = layoutColor;
            if (cr->culled)
                semanticColor = culledColor;
            else if (world.HasComponent<UICanvas>(entry.e))
                semanticColor = canvasColor;
            else if (const UIInteractable* interactable =
                         world.GetComponent<UIInteractable>(entry.e);
                     interactable && interactable->raycastTarget
                         && !interactable->disabled)
                semanticColor = interactColor;

            // Draw an owning RectMask's effective (inherited-intersected) clip.
            // For the selected node, also show the actual visible/hit-test area
            // even when the clip belongs to an ancestor.
            const UIClipRect* ownClip = world.GetComponent<UIClipRect>(entry.e);
            const Rect visibleRect = IntersectRects(cr->rect, cr->clipRect);
            if ((ownClip && ownClip->enabled)
                || (selected && !sameRect(visibleRect, cr->rect)))
                addOutline(selected ? visibleRect : cr->clipRect,
                           clipColor, 1.5f, selected);

            addOutline(cr->rect, selected ? selectedColor : semanticColor,
                       selected ? 2.5f : 1.f, selected);

            if (selected)
            {
                // Pivot is expressed inside the final resolved rect, so this
                // marker is the real transform/layout pivot after scaling.
                if (const UIRect* authored = world.GetComponent<UIRect>(entry.e))
                {
                    const Vec2 pivot = Clamp01(authored->pivot);
                    const Vec2 pivotPoint{
                        cr->rect.mn.x + pivot.x * cr->rect.Width(),
                        cr->rect.mn.y + pivot.y * cr->rect.Height()
                    };
                    addCross(pivotPoint, selectedColor, 6.f, 2.f, true);
                }
            }
        }
        drawList.PopTexture();
    }

    // ---- Sprite-sheet animation ----
    void AdvanceSpriteAnimations(World& world, float dt)
    {
        auto* pool = world.GetPool<UISpriteAnimComponent>();
        if (!pool) return;

        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        for (size_t i = 0; i < data.size(); ++i)
        {
            UISpriteAnimComponent& a = data[i];
            const int cols  = a.columns > 0 ? a.columns : 1;
            const int rows  = a.rows    > 0 ? a.rows    : 1;
            int       total = a.frameCount > 0 ? a.frameCount : cols * rows;
            if (total < 1) total = 1;

            if (a.playing && a.fps > 0.f && total > 1)
            {
                a.elapsed += dt;
                const int step = static_cast<int>(a.elapsed * a.fps);
                if (a.pingpong)
                {
                    const int period = 2 * (total - 1);
                    const int p = step % period;
                    a.frame = (p < total) ? p : (period - p);
                }
                else if (a.loop)
                {
                    a.frame = step % total;
                }
                else
                {
                    a.frame = step < total ? step : (total - 1);
                }
            }
            a.frame = std::clamp(a.frame, 0, total - 1);

            // Current frame's grid sub-rect.
            const int   col = a.frame % cols;
            const int   row = a.frame / cols;
            const float u0  = static_cast<float>(col)     / static_cast<float>(cols);
            const float v0  = static_cast<float>(row)     / static_cast<float>(rows);
            const float u1  = static_cast<float>(col + 1) / static_cast<float>(cols);
            const float v1  = static_cast<float>(row + 1) / static_cast<float>(rows);

            const Entity e = ents[i];
            if (auto* img = world.GetComponent<UIImage>(e))
            {
                img->uv0 = { u0, v0 };
                img->uv1 = { u1, v1 };
            }
        }
    }

} // namespace UI
