#include "ECS/TODSystems.h"
#include "ECS/TODComponents.h"
#include "ECS/Components.h"   // LightData, LightType
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)
#include "ECS/HierarchyComponents.h"  // LocalTransform / GlobalTransform / Parent

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace DirectX;

namespace TODUtil
{
    TODConfigComponent* FindConfig(World& w)
    {
        auto* p = w.GetPool<TODConfigComponent>();
        if (!p || p->Data().empty()) return nullptr;
        return &p->Data()[0];
    }
    TODOutputComponent* FindOutput(World& w)
    {
        auto* p = w.GetPool<TODOutputComponent>();
        if (!p || p->Data().empty()) return nullptr;
        return &p->Data()[0];
    }
}

void TODTickSystem::Update(World& w, float dt)
{
    auto* cfg = TODUtil::FindConfig(w);
    if (!cfg || !cfg->enabled || cfg->timeSpeed == 0.0f) return;
    cfg->timeOfDay += cfg->timeSpeed * dt;
    cfg->timeOfDay -= std::floor(cfg->timeOfDay);   // wrap to [0,1)
}

void TODEvaluationSystem::Update(World& w)
{
    auto* cfg = TODUtil::FindConfig(w);
    auto* out = TODUtil::FindOutput(w);
    if (!cfg || !out) return;
    if (!cfg->enabled)
    {
        out->dirty = false;   // outputs are not live while TOD is off
        return;
    }

    // ---- Sun direction from hour angle + latitude (declination = 0) ----
    const float H    = (cfg->timeOfDay - 0.5f) * (2.0f * 3.14159265f);
    const float cosH = std::cos(H);
    const float sinH = std::sin(H);
    const float cosL = std::cos(cfg->latitudeRad);
    const float sinL = std::sin(cfg->latitudeRad);

    XMFLOAT3 sunDir;
    sunDir.x = -sinH;            // east at sunrise side
    sunDir.y = cosL * cosH;      // altitude
    sunDir.z = -sinL * cosH;     // north-south
    const float len = std::sqrt(sunDir.x*sunDir.x + sunDir.y*sunDir.y + sunDir.z*sunDir.z);
    if (len > 1e-6f) { sunDir.x /= len; sunDir.y /= len; sunDir.z /= len; }

    const bool sunBelowHorizon = (sunDir.y < 0.0f);

    // ---- Geometric sun (for the atmosphere shaders) ----
    // Below the horizon -> colour zero, so scattering goes dark and no blue daytime dome lingers at
    // night.
    out->sunDirection = sunDir;
    if (sunBelowHorizon)
    {
        out->sunColor = { 0.0f, 0.0f, 0.0f };
    }
    else
    {
        const float elev      = sunDir.y;
        const float daylight  = std::max(0.0f, elev);
        const float tw        = std::exp(-std::max(0.0f, -elev) * 8.0f);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * (0.05f + 0.95f * daylight) * tw
                              * cfg->sunBrightnessScale;
        const float warmth    = 1.0f - std::min(1.0f, daylight * 2.0f);
        out->sunColor = {
            intensity,
            intensity * (1.00f - 0.30f * warmth),
            intensity * (1.00f - 0.55f * warmth) };
    }

    // ---- Moon = -sun; the disk shows slightly past the horizon for a smooth fade ----
    out->moonDirection = { -sunDir.x, -sunDir.y, -sunDir.z };
    out->moonDiskVisible = (out->moonDirection.y > 0.01f);

    // Moon disk colour: constant cool blue-white so it stays readable through
    // its whole arc. Intensity fade is in the shader's horizon mask.
    constexpr float kMoonDiskTint = 0.1f;
    out->moonColor = { 0.55f * kMoonDiskTint,
                       0.70f * kMoonDiskTint,
                       1.00f * kMoonDiskTint };

    // ---- Active body: sun by day, moon by night, smooth ramp across 0 ----
    auto smoothstep01 = [](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    constexpr float kBand = 0.05f;   // ~ 2.9 deg
    const float sunT  = smoothstep01( sunDir.y / kBand);
    const float moonT = smoothstep01(-sunDir.y / kBand);

    XMFLOAT3 sunCol { 0, 0, 0 };
    {
        const float elev      = std::max(0.0f, sunDir.y);
        const float tw        = std::exp(-std::max(0.0f, -sunDir.y) * 8.0f);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * (0.05f + 0.95f * elev) * tw
                              * cfg->sunBrightnessScale;
        const float warmth    = 1.0f - std::min(1.0f, elev * 2.0f);
        sunCol.x = intensity;
        sunCol.y = intensity * (1.0f - 0.30f * warmth);
        sunCol.z = intensity * (1.0f - 0.55f * warmth);
    }

    XMFLOAT3 moonCol { 0, 0, 0 };
    {
        const float moonElev  = -sunDir.y;
        const float moonlight = std::max(0.0f, moonElev);
        const float basePeak  = 5.0f;
        const float intensity = basePeak * cfg->moonIntensityScale
                              * (0.75f + 0.25f * moonlight)
                              * cfg->sunBrightnessScale;
        moonCol.x = intensity * 0.55f;
        moonCol.y = intensity * 0.70f;
        moonCol.z = intensity * 1.00f;
    }

    out->isMoonActive    = sunBelowHorizon;
    out->activeDirection = sunBelowHorizon
        ? XMFLOAT3{ -sunDir.x, -sunDir.y, -sunDir.z }
        : sunDir;
    out->activeColor.x = sunCol.x * sunT + moonCol.x * moonT;
    out->activeColor.y = sunCol.y * sunT + moonCol.y * moonT;
    out->activeColor.z = sunCol.z * sunT + moonCol.z * moonT;
    out->dirty = true;
}

template <typename Tag>
static Entity FirstEntityWithTag(World& w)
{
    auto* p = w.GetPool<Tag>();
    if (!p || p->Entities().empty()) return NullEntity;
    return p->Entities()[0];
}

// Aims a TOD-driven light entity down @p travelDir (the light travel direction, body -> ground).
// LightData has no direction, so TOD steers the light by writing the entity's rotation, like a
// gizmo drag or an animation track. These systems run inside Renderer::SyncSkyboxIBL, after
// TransformPropagateSystem composed this frame's GlobalTransform, so both are written:
// LocalTransform (authoritative, serialized) and GlobalTransform (later readers this frame see the
// new orientation). Same pattern as DecalSpawner. Composes with the parent when not a root.
static void AimLightEntity(World& w, Entity e, const DirectX::XMFLOAT3& travelDir)
{
    using namespace DirectX;
    const XMFLOAT4 q = LightUtil::RotationAiming(travelDir);

    LocalTransform* lt = w.GetComponent<LocalTransform>(e);
    if (lt) lt->rotation = q;

    GlobalTransform* gt = w.GetComponent<GlobalTransform>(e);
    if (!gt) return;

    XMMATRIX local = lt ? lt->ToMatrix() : XMMatrixRotationQuaternion(XMLoadFloat4(&q));
    if (const Parent* p = w.GetComponent<Parent>(e); p && p->entity != NullEntity)
    {
        if (const GlobalTransform* pg = w.GetComponent<GlobalTransform>(p->entity))
            local = XMMatrixMultiply(local, XMLoadFloat4x4(&pg->matrix));
    }
    XMStoreFloat4x4(&gt->matrix, local);
}

void TODSunSyncSystem::Update(World& w)
{
    auto* out = TODUtil::FindOutput(w);
    auto* cfg = TODUtil::FindConfig(w);
    if (!cfg || !out || !cfg->enabled) return;

    Entity e = FirstEntityWithTag<SunLightTag>(w);
    if (e == NullEntity) return;
    LightData* ld = w.GetComponent<LightData>(e);
    if (!ld) return;

    ld->type = LightType::Directional;
    // sunDirection points towards the sun; a light travels sun -> ground, so negate it before aiming.
    AimLightEntity(w, e, { -out->sunDirection.x, -out->sunDirection.y, -out->sunDirection.z });

    if (out->isMoonActive)
    {
        // Sun below the horizon - the moon entity drives lighting, zero sun contribution.
        ld->color     = { 0.f, 0.f, 0.f };
        ld->intensity = 0.f;
    }
    else
    {
        // activeColor already bakes in basePeak * brightness ramp + warmth.
        ld->color     = out->activeColor;
        ld->intensity = 1.f;
    }
}

void TODMoonSyncSystem::Update(World& w)
{
    auto* out = TODUtil::FindOutput(w);
    auto* cfg = TODUtil::FindConfig(w);
    if (!cfg || !out || !cfg->enabled) return;

    Entity e = FirstEntityWithTag<MoonLightTag>(w);
    if (e == NullEntity) return;
    LightData* ld = w.GetComponent<LightData>(e);
    if (!ld) return;

    ld->type = LightType::Directional;
    AimLightEntity(w, e, { -out->moonDirection.x, -out->moonDirection.y, -out->moonDirection.z });

    if (out->isMoonActive)
    {
        ld->color     = out->activeColor;
        ld->intensity = 1.f;
    }
    else
    {
        ld->color     = { 0.f, 0.f, 0.f };
        ld->intensity = 0.f;
    }
}

void TODAtmosphereSyncSystem::Update(World&)
{
    // Deliberate no-op: AtmosphereComponent fields are author-owned. Kept as the hook for TOD-driven
    // atmosphere parameters (sky tint curve, dawn / dusk mist density).
}

// ==== TODLightSyncSystem: local lights and their bulb emissive, driven by the sun ====
namespace
{
    // 0 at altitude >= hi, 1 at altitude <= lo, smooth between. `hi` is ABOVE
    // `lo` because the lamp comes on as the sun goes DOWN.
    float FallingSmoothstep(float hi, float lo, float x)
    {
        if (hi == lo) return (x <= lo) ? 1.f : 0.f;
        float u = (hi - x) / (hi - lo);
        u = std::clamp(u, 0.f, 1.f);
        return u * u * (3.f - 2.f * u);
    }

    std::string ToLowerAscii(std::string v)
    {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    }

    // Depth-first search of @p root's descendants for the first mesh entity whose material came from
    // a .imat matching @p lowerNeedle (case-insensitive substring). Not a name lookup:
    // SceneInstanceLoader names every mesh child of a node `<node>_mesh`, so only the material tells a
    // lamp's pole from its bulb.
    Entity FindDescendantWithMaterial(World& w, Entity root, const std::string& lowerNeedle)
    {
        if (root == NullEntity || lowerNeedle.empty()) return NullEntity;

        std::vector<Entity>        stack{ root };
        std::unordered_set<Entity> seen{ root };
        while (!stack.empty())
        {
            const Entity cur = stack.back();
            stack.pop_back();

            if (cur != root)
                if (const auto* sp = w.GetComponent<MaterialSourcePath>(cur))
                    if (ToLowerAscii(sp->path).find(lowerNeedle) != std::string::npos)
                        return cur;

            // Visited-guarded so a malformed Children graph terminates instead
            // of hanging the render phase this runs in.
            if (const Children* kids = w.GetComponent<Children>(cur))
                for (Entity c : kids->entities)
                    if (w.IsAlive(c) && seen.insert(c).second)
                        stack.push_back(c);
        }
        return NullEntity;
    }
}

void TODLightSyncSystem::Update(World& w, float dt)
{
    auto* pool = w.GetPool<TODLightComponent>();
    if (!pool || pool->Entities().empty()) return;

    const TODConfigComponent* cfg = TODUtil::FindConfig(w);
    const TODOutputComponent* out = TODUtil::FindOutput(w);
    // No TOD authority, or it is switched off: leave every lamp exactly as
    // authored. Forcing them to a computed value here would silently overwrite
    // hand-set intensities in a scene that never asked for a day/night cycle.
    if (!cfg || !out || !cfg->enabled) return;

    // sunDirection is unit length (TODEvaluationSystem), so .y IS sin(elevation).
    const float    altitude = out->sunDirection.y;
    const uint32_t epoch    = w.Epoch();

    // ---- Resolve pass ----
    // At most once per world epoch, and only when something still needs it: resolving per lamp would
    // be a full-world name scan each (170 lamps x ~15k entities per frame), so build one shared index.
    bool needResolve = false;
    w.ForEach<TODLightComponent>([&](Entity, TODLightComponent& c)
    {
        if (c.enabled && !c.bulbMaterial.empty() && c.resolvedEpoch != epoch)
            needResolve = true;
    });

    std::unordered_map<std::string, Entity> nameIndex;
    if (needResolve)
        for (Entity e : w.GetEntities())
            if (w.IsAlive(e))
                nameIndex.emplace(w.GetName(e), e);   // first wins, like FindByName

    // ---- Drive ----
    w.ForEach<TODLightComponent>([&](Entity e, TODLightComponent& c)
    {
        if (!c.enabled) return;

        const float target = FallingSmoothstep(c.duskStart, c.duskEnd, altitude);

        // First sight SNAPS to the target instead of fading up from zero: a
        // lamp authored lit, in a scene that loads at night, must not begin
        // every session by blinking off and back on.
        if (c.blend < 0.f)
        {
            c.blend = target;
        }
        else if (c.blend != target)
        {
            const float step = (c.warmupTime > 0.f) ? (dt / c.warmupTime) : 1.f;
            c.blend = (c.blend < target) ? std::min(target, c.blend + step)
                                         : std::max(target, c.blend - step);
        }

        // Quantize the blend, not the two outputs: that keeps light and glow in lockstep, and it is the
        // cheap end - 64 steps for the whole fade however large emissiveOn is.
        const float kq = std::floor(c.blend * 64.f + 0.5f) / 64.f;

        // Lights are re-gathered from the pool every frame with no dirty flag
        // and no cache to bust, so this write is free.
        if (LightData* ld = w.GetComponent<LightData>(e))
            ld->intensity = c.onIntensity * kq;

        if (c.bulbMaterial.empty()) return;

        if (c.resolvedEpoch != epoch)
        {
            c.resolvedEpoch = epoch;
            c.resolvedBulb  = NullEntity;

            Entity root = NullEntity;
            if (c.lampName.empty())
            {
                if (const Parent* p = w.GetComponent<Parent>(e)) root = p->entity;
            }
            else
            {
                auto it = nameIndex.find(c.lampName);
                if (it != nameIndex.end()) root = it->second;
            }
            c.resolvedBulb = FindDescendantWithMaterial(w, root, ToLowerAscii(c.bulbMaterial));
            // "Not found" is latched by resolvedEpoch, not retried every frame: a wrong lampName costs one
            // scan per world load.
        }

        if (c.resolvedBulb == NullEntity || !w.IsAlive(c.resolvedBulb)) return;
        MaterialComponent* mc = w.GetComponent<MaterialComponent>(c.resolvedBulb);
        if (!mc) return;

        const float emissive = c.emissiveOff + (c.emissiveOn - c.emissiveOff) * kq;

        // Compare before writing: MaterialComponent::SetEmissiveStrength calls SetDirty()
        // unconditionally (unlike the Lua property setter), and a non-empty m_sceneMatDirty is a global
        // term in both the raster retained-candidate-cache gate and the DDGI / RT retained-gather gate, so
        // one lamp rewriting the same value would re-gather the whole world every frame (~2 ms/frame at
        // Tokyo's entity count).
        if (mc->emissiveColor.w != emissive)
        {
            mc->SetEmissiveStrength(emissive);
            c.lastEmissive = emissive;
        }
    });
}
