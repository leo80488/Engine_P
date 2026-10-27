#include "UI/WorldSpaceUISystem.h"
#include "UI/WorldSpaceUI.h"
#include "UI/FontLibrary.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace DirectX;

namespace UI
{
    // Project (x, y, z, 1) by a row-major DirectX view-proj matrix (row vectors: clip =
    // pos x M) and return clip.w, the signed camera-space depth (<= 0 = behind the camera).
    static float ProjectToCameraDepth(const XMFLOAT4X4& m,
                                       float x, float y, float z)
    {
        const float* p = reinterpret_cast<const float*>(&m);
        // Flat row-major p[r * 4 + c]: clip.w = pos x column 3 of M.
        return p[3]*x + p[7]*y + p[11]*z + p[15];
    }

    // One view's fade and scale of @p c at world position (wx, wy, wz); false = culled (@p alpha and
    // @p scale untouched).
    static bool EvaluateForView(const WorldSpaceUIComponent& c, float wx, float wy, float wz,
                                const WorldSpaceUIView& view, float& alpha, float& scale)
    {
        const float depth = ProjectToCameraDepth(view.viewProjMatrix, wx, wy, wz);
        if (depth <= 0.f && c.hideWhenBehindCamera) return false;

        if (c.fadeNear > 0.f && depth < c.fadeNear) return false;
        if (c.fadeFar  > 0.f && depth > c.fadeFar)  return false;

        // computedScale multiplies baseSize. ConstantWorld: 1 (the projection already sizes the
        // world quad). ConstantPixel: a multiplier that keeps the projected pixel size constant at
        // any depth.
        if (c.scalingMode == ScalingMode::ConstantPixel)
        {
            // pixelsPerWorld = (canvasH / 2) / (depth * tan(fov / 2)); the scale 1 / pixelsPerWorld
            // makes baseSize a pixel size. The view carries no FOV, so assume the 60 deg default: a
            // small error is acceptable since users tune baseSize anyway.
            constexpr float kHalfFovTan = 0.5773f; // tan(30 deg): the engine's default 60 deg FOV
            const float pixelsPerWorld =
                (view.canvasSize.y * 0.5f) / (depth * kHalfFovTan);
            if (pixelsPerWorld > 0.0001f)
                scale = 1.0f / pixelsPerWorld;
            else
                scale = 1.0f;
        }
        else // ConstantWorld
        {
            scale = 1.0f;
        }

        // Soft fade instead of popping: linear ramps inside the endpoints (far band
        // max(0.5, 10% of fadeFar), near band max(0.5, 50% of fadeNear)).
        float a = 1.0f;
        if (c.fadeFar > 0.f)
        {
            const float band = std::max(0.5f, c.fadeFar * 0.1f);
            if (depth > c.fadeFar - band)
                a *= std::max(0.f, (c.fadeFar - depth) / band);
        }
        if (c.fadeNear > 0.f)
        {
            const float band = std::max(0.5f, c.fadeNear * 0.5f);
            if (depth < c.fadeNear + band)
                a *= std::max(0.f, (depth - c.fadeNear) / band);
        }
        alpha = a;
        return true;
    }

    void WorldSpaceUISystem::Tick(World& world, const WorldSpaceUIView& view, float dt,
                                  const WorldSpaceUIViewSlot* views, uint32_t viewCount)
    {
        // ---- WorldSpaceUI fade / scale / cull ----
        if (auto* pool = world.GetPool<WorldSpaceUIComponent>())
        {
            const auto& ents = pool->Entities();
            auto&       data = pool->Data();
            for (size_t i = 0; i < data.size(); ++i)
            {
                WorldSpaceUIComponent& c = data[i];
                c.isCulled      = true;        // default: don't render
                c.computedAlpha = 0.0f;
                c.computedScale = 1.0f;
                c.viewCulled    = 0xFF;
                for (uint32_t k = 0; k < WorldSpaceUIComponent::kViewCount; ++k)
                {
                    c.viewAlpha[k] = 0.0f;
                    c.viewScale[k] = 1.0f;
                }

                if (!view.valid && viewCount == 0) continue;

                const Entity e = ents[i];
                const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
                if (!gt) continue;

                // Add the DamageNumber's per-frame offset if present.
                float wx = gt->matrix.m[3][0];
                float wy = gt->matrix.m[3][1];
                float wz = gt->matrix.m[3][2];
                if (auto* dn = world.GetComponent<DamageNumberComponent>(e))
                {
                    wx += dn->currentOffset.x;
                    wy += dn->currentOffset.y;
                    wz += dn->currentOffset.z;
                }

                if (view.valid && EvaluateForView(c, wx, wy, wz, view, c.computedAlpha, c.computedScale))
                    c.isCulled = false;
                for (uint32_t v = 0; v < viewCount; ++v)
                {
                    const uint32_t k = views[v].slot - 1;
                    if (k >= WorldSpaceUIComponent::kViewCount || !views[v].view.valid) continue;
                    if (EvaluateForView(c, wx, wy, wz, views[v].view, c.viewAlpha[k], c.viewScale[k]))
                        c.viewCulled = static_cast<uint8_t>(c.viewCulled & ~(1u << k));
                }
            }
        }

        // ---- DamageNumber lifecycle ----
        // Integrate velocity into currentOffset, count lifetime down, fade through the
        // computedAlpha multiplier, destroy expired entries.
        if (auto* dnPool = world.GetPool<DamageNumberComponent>())
        {
            const auto& ents = dnPool->Entities();
            auto&       data = dnPool->Data();
            std::vector<Entity> expired;
            expired.reserve(8);

            for (size_t i = 0; i < data.size(); ++i)
            {
                DamageNumberComponent& dn = data[i];
                dn.lifetime -= dt;
                dn.currentOffset.x += dn.velocity.x * dt;
                dn.currentOffset.y += dn.velocity.y * dt;
                dn.currentOffset.z += dn.velocity.z * dt;

                if (dn.lifetime <= 0.f)
                {
                    expired.push_back(ents[i]);
                    continue;
                }

                // Pop in over the first 1/6 of the lifetime and fade out over the last 30%, multiplied
                // into the WorldSpaceUI's computedAlpha.
                if (auto* ws = world.GetComponent<WorldSpaceUIComponent>(ents[i]))
                {
                    const float t = dn.lifetime / std::max(0.001f, dn.totalLifetime);
                    const float fadeIn = std::min(1.0f, (1.0f - t) * 6.0f); // pop-in over the first 1/6
                    const float fadeOut = std::clamp(t / 0.3f, 0.0f, 1.0f);  // dissolve last 30%
                    ws->computedAlpha *= std::min(fadeIn, fadeOut);
                    for (float& a : ws->viewAlpha) a *= std::min(fadeIn, fadeOut);
                }
            }

            for (Entity e : expired)
                world.DestroyEntity(e);
        }

        // ---- World-text glyph residency (CPU phase) ----
        // Resolve each text's font and bake missing glyphs now: the billboard pass lays the text
        // out right after this tick (WorldUIBillboardPass::BuildLane via Renderer::ExtractWorldUI
        // on the game half) with bake-free FindGlyph lookups only. A missing glyph is RESERVED here
        // (cell and metrics final); its pixels are rendered in parallel with the frame's other new
        // glyphs and uploaded by FontLibrary::FlushPendingUploads in the App loop, before the graph
        // runs.
        if (auto* txtPool = world.GetPool<WorldUITextComponent>())
        {
            for (auto& t : txtPool->Data())
            {
                if (!t.visible || t.text.empty()) { t.customShaderSlotRt = 0; continue; }
                FontLibrary::Get().Resolve(t.fontPath).EnsureGlyphs(t.text.c_str());
                // Resolve the custom-shader slot here: RegisterTextShader mutates a global table the
                // render passes read while the graph records, so all mutation stays in the CPU phase.
                t.customShaderSlotRt = t.customShaderPath.empty()
                                     ? 0u : RegisterTextShader(t.customShaderPath);
            }
        }
        if (auto* dnPool = world.GetPool<DamageNumberComponent>())
        {
            for (auto& dn : dnPool->Data())
            {
                if (dn.text.empty()) { dn.customShaderSlotRt = 0; continue; }
                FontLibrary::Get().Resolve(dn.fontPath).EnsureGlyphs(dn.text.c_str());
                dn.customShaderSlotRt = dn.customShaderPath.empty()
                                      ? 0u : RegisterTextShader(dn.customShaderPath);
            }
        }
    }

} // namespace UI
