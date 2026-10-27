#include "Editor/DebugDrawSystem.h"
#include "System/CommandLine.h"

#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/DebugIconPass.h"
#include "Physics/PhysicsSystem.h"
#include "Tools/CollisionMeshBaker.h"
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)
#include "ECS/HierarchyComponents.h"
#include "ECS/FrameContext.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "ECS/LocalFogVolumeComponent.h"
// VFX emitter components: invisible in the editor when idle, so they get a
// gizmo icon to stay selectable.
#include "ECS/ParticleComponent.h"     // ParticleEmitterComponent
#include "ECS/TrailComponent.h"        // TrailComponent
#include "ECS/BeamComponent.h"         // BeamComponent

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

// ==== Debug icon-kind registry ====
// The single place that defines WHAT debug icons exist and HOW they map to
// entities. To add an icon kind: add a DebugCategory bit (DebugCategory.h) and
// ONE row to kIconKinds[] (a Font Awesome glyph; a .itex only when no glyph
// fits). Everything else (GPU pass, glyph baking / texture loading, menu toggle)
// is generic.
namespace
{
    // Emit one icon at every entity that owns component T, at its world
    // position. Generic over the marker component (LightData, CameraComponent, ...).
    template <class T>
    void EmitIconsForPool(World& world, DebugIconPass& icons,
                          uint32_t texIdx, float halfSize)
    {
        auto* pool = world.GetPool<T>();
        if (!pool) return;
        const auto& ents = pool->Entities();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const Entity e = ents[i];
            if (!world.IsAlive(e)) continue;
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;
            icons.AddIcon({ gt->matrix._41, gt->matrix._42, gt->matrix._43 },
                          halfSize, texIdx);
        }
    }

    // The entity the viewport is currently looking THROUGH this frame (set by
    // Submit from ctx.cameraEntity). An icon at your own eye projects
    // degenerately (near-plane smear), so EmitCameraIcons skips exactly it.
    Entity s_viewCameraEntity = NullEntity;

    // Camera icons. Skips only the camera the viewport looks through and the
    // editor free cam, not every CameraControllerComponent: the scene's Main
    // Camera keeps its controller while the editor looks through its own
    // transient camera, and its icon must still show.
    void EmitCameraIcons(World& world, DebugIconPass& icons,
                         uint32_t texIdx, float halfSize)
    {
        auto* pool = world.GetPool<CameraComponent>();
        if (!pool) return;
        const auto& ents = pool->Entities();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const Entity e = ents[i];
            if (!world.IsAlive(e)) continue;
            if (e == s_viewCameraEntity) continue;              // looking through it
            if (world.HasComponent<EditorCameraTag>(e)) continue; // editor free cam
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;
            icons.AddIcon({ gt->matrix._41, gt->matrix._42, gt->matrix._43 }, halfSize, texIdx);
        }
    }

    // VFX emitter icons: one icon per entity carrying any VFX emitter
    // component (particle / trail / beam).
    //
    // Iterates the three EMITTER pools, not the transform pool: walking every
    // GlobalTransform with three HasComponent<> each costs ~45,000 unordered_map
    // lookups per frame on a 15k-entity city to emit ZERO icons, and this runs by
    // default (iconsEnabled) outside any Tracy zone. N is the emitter count (tens),
    // not the world size. An entity with several VFX components still gets a single
    // icon; the de-dup HasComponent runs only on entities found in a later pool.
    void EmitVFXIcons(World& world, DebugIconPass& icons,
                      uint32_t texIdx, float halfSize)
    {
        const auto emitOne = [&](Entity e)
        {
            if (!world.IsAlive(e)) return;
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) return;
            icons.AddIcon({ gt->matrix._41, gt->matrix._42, gt->matrix._43 },
                          halfSize, texIdx);
        };

        if (auto* pParticle = world.GetPool<ParticleEmitterComponent>())
            for (Entity e : pParticle->Entities())
                emitOne(e);

        if (auto* pTrail = world.GetPool<TrailComponent>())
            for (Entity e : pTrail->Entities())
                if (!world.HasComponent<ParticleEmitterComponent>(e))
                    emitOne(e);

        if (auto* pBeam = world.GetPool<BeamComponent>())
            for (Entity e : pBeam->Entities())
                if (!world.HasComponent<ParticleEmitterComponent>(e) &&
                    !world.HasComponent<TrailComponent>(e))
                    emitOne(e);
    }

    // ---- Post-process volume gizmos ----
    using namespace DirectX;

    // Draw an OBB (center+rotation+half-extents) as 12 edges.
    void DrawOBB(DebugWirePass& dbg, FXMVECTOR center, FXMVECTOR rotQ,
                 const XMFLOAT3& half, uint32_t color)
    {
        XMFLOAT3 c[8];
        for (int i = 0; i < 8; ++i)
        {
            const float sx = (i & 1) ? 1.f : -1.f;
            const float sy = (i & 2) ? 1.f : -1.f;
            const float sz = (i & 4) ? 1.f : -1.f;
            XMVECTOR local = XMVectorSet(sx * half.x, sy * half.y, sz * half.z, 0.f);
            XMVECTOR world = XMVectorAdd(center, XMVector3Rotate(local, rotQ));
            XMStoreFloat3(&c[i], world);
        }
        static const int kEdges[12][2] = {
            {0,1},{2,3},{4,5},{6,7},  // x edges
            {0,2},{1,3},{4,6},{5,7},  // y edges
            {0,4},{1,5},{2,6},{3,7},  // z edges
        };
        for (auto& edge : kEdges) dbg.AddLine(c[edge[0]], c[edge[1]], color);
    }

    // ---- Line sinks ----
    // Colour packing note: DebugWire.vs.hlsl unpacks the vertex colour as
    // 0xAABBGGRR (R = low byte). Only the ALPHA position matters below, and
    // that is the high byte under either reading.
    inline uint32_t WithAlpha(uint32_t color, uint32_t alpha)
    {
        return (color & 0x00FFFFFFu) | (alpha << 24);
    }

    // Always-on-top sink, for overlays that must be visible through geometry
    // (post-process volume bounds).
    struct OverlayLine
    {
        DebugWirePass& dbg;
        void operator()(const XMFLOAT3& a, const XMFLOAT3& b, uint32_t c) const
        {
            dbg.AddLine(a, b, c);
        }
    };

    // Light-gizmo sink: depth-AWARE lines, full alpha where visible, faded where
    // scene geometry is nearer (the fade is a pixel-shader decision in
    // DebugWireDepth.ps.hlsl, so one edge = one line). Occluding walls and floors
    // pin the gizmo to the scene; drawn always on top it gives the eye no depth
    // cue and seems to slide around with the camera.
    struct XRayLine
    {
        DebugWirePass& dbg;
        void operator()(const XMFLOAT3& a, const XMFLOAT3& b, uint32_t c) const
        {
            dbg.AddLineDepthTested(a, b, c);
        }
    };

    inline XMFLOAT3 StoreF3(XMVECTOR v) { XMFLOAT3 f; XMStoreFloat3(&f, v); return f; }

    // Any axis not parallel to `d`: seed for an orthonormal basis around it.
    inline XMVECTOR PerpSeed(XMVECTOR d)
    {
        return (std::fabs(XMVectorGetY(d)) > 0.99f) ? XMVectorSet(1, 0, 0, 0)
                                                    : XMVectorSet(0, 1, 0, 0);
    }

    // Draw a sphere as three great-circle rings.
    template <class Emit>
    void DrawSphere(const Emit& emit, const XMFLOAT3& center, float radius, uint32_t color)
    {
        constexpr int kSeg = 24;
        for (int axis = 0; axis < 3; ++axis)
        {
            XMFLOAT3 prev{};
            for (int s = 0; s <= kSeg; ++s)
            {
                const float a = (float(s) / kSeg) * XM_2PI;
                const float u = std::cos(a) * radius;
                const float v = std::sin(a) * radius;
                XMFLOAT3 p = center;
                if (axis == 0)      { p.x += u; p.y += v; }
                else if (axis == 1) { p.y += u; p.z += v; }
                else                { p.x += u; p.z += v; }
                if (s > 0) emit(prev, p, color);
                prev = p;
            }
        }
    }

    // ---- Spot-light gizmo ----
    // Shape: the lit volume is the attenuation SPHERE (radius = range) clipped
    // by the cone of half-angle `halfAngle` (DDGIRayTrace.cs.hlsl:
    // `dist > light.radius` -> reject, then the cos(spotAngle) test; Lighting and
    // Cluster use the same rule). So the far end is a spherical CAP, not a flat
    // disc at `range`, whose rim would sit at range/cos(angle) - up to 2x past
    // the actual cutoff on a wide cone.
    //
    // Legibility: a base ring + 4 spokes collapses to an ellipse with a few
    // crossing lines from most angles, and its apparent shape swings as the
    // camera orbits (it reads as "the gizmo is following the camera"). Hence
    // redundant, mutually-parallaxing structure:
    //   * cross rings down the axis -> the strongest single depth cue,
    //   * meridian arcs on the cap  -> the far end reads as a dome, not a lid,
    //   * 8 spokes                  -> a usable silhouette from any view angle,
    //   * axis line + apex cross    -> states exactly WHERE the light sits.
    template <class Emit>
    void DrawSpotGizmo(const Emit& emit, const XMFLOAT3& apex, const XMFLOAT3& dir,
                       float halfAngle, float range, uint32_t color)
    {
        if (range <= 1e-4f) return;
        // Clamp to (0, 89] deg: at 90 deg the cap degenerates into a hemisphere
        // edge and the basis math loses precision.
        const float theta = std::clamp(halfAngle, 0.01f, 1.5533f);
        const float cosT  = std::cos(theta);
        const float sinT  = std::sin(theta);

        const XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&dir));
        const XMVECTOR o = XMLoadFloat3(&apex);
        const XMVECTOR u = XMVector3Normalize(XMVector3Cross(PerpSeed(d), d));
        const XMVECTOR v = XMVector3Cross(d, u);

        // Point on the cone surface at slant distance `s` from the apex (so
        // s == range lands exactly on the attenuation sphere), `a` around the axis.
        auto conePoint = [&](float s, float a)
        {
            return XMVectorAdd(o,
                XMVectorAdd(XMVectorScale(d, s * cosT),
                    XMVectorAdd(XMVectorScale(u, std::cos(a) * s * sinT),
                                XMVectorScale(v, std::sin(a) * s * sinT))));
        };

        // Interior structure is dimmer so the outer boundary stays the shape
        // the eye reads first.
        const uint32_t dim = WithAlpha(color, 0x80u);

        // Cross rings. Only t = 1 is the real range boundary; the rest exist
        // purely as parallax anchors along the axis.
        constexpr int   kSeg      = 32;
        constexpr float kRingT[]  = { 0.25f, 0.5f, 0.75f, 1.0f };
        for (float t : kRingT)
        {
            const uint32_t c = (t >= 1.0f) ? color : dim;
            XMFLOAT3 prev = StoreF3(conePoint(range * t, 0.f));
            for (int s = 1; s <= kSeg; ++s)
            {
                const XMFLOAT3 p = StoreF3(conePoint(range * t, (float(s) / kSeg) * XM_2PI));
                emit(prev, p, c);
                prev = p;
            }
        }

        // Spokes apex -> rim.
        constexpr int kSpokes = 8;
        const XMFLOAT3 apexF = StoreF3(o);
        for (int s = 0; s < kSpokes; ++s)
            emit(apexF, StoreF3(conePoint(range, (float(s) / kSpokes) * XM_2PI)), color);

        // Cap dome: meridian arcs from the axis tip out to the rim.
        const XMFLOAT3 axisTip = StoreF3(XMVectorAdd(o, XMVectorScale(d, range)));
        constexpr int kMeridians = 4;
        constexpr int kArcSeg    = 8;
        for (int m = 0; m < kMeridians; ++m)
        {
            const float    a      = (float(m) / kMeridians) * XM_2PI;
            const XMVECTOR radial = XMVectorAdd(XMVectorScale(u, std::cos(a)),
                                                XMVectorScale(v, std::sin(a)));
            XMFLOAT3 prev = axisTip;
            for (int s = 1; s <= kArcSeg; ++s)
            {
                const float ang = theta * (float(s) / kArcSeg);
                const XMFLOAT3 p = StoreF3(XMVectorAdd(o,
                    XMVectorAdd(XMVectorScale(d,      range * std::cos(ang)),
                                XMVectorScale(radial, range * std::sin(ang)))));
                emit(prev, p, dim);
                prev = p;
            }
        }

        // Axis + apex marker: "the light is HERE, aimed THAT way". The apex is
        // the one thing the user wants to locate; the spokes only imply it.
        emit(apexF, axisTip, dim);
        const float k = std::min(range * 0.05f, 0.25f);
        emit({ apexF.x - k, apexF.y, apexF.z }, { apexF.x + k, apexF.y, apexF.z }, color);
        emit({ apexF.x, apexF.y - k, apexF.z }, { apexF.x, apexF.y + k, apexF.z }, color);
        emit({ apexF.x, apexF.y, apexF.z - k }, { apexF.x, apexF.y, apexF.z + k }, color);
    }

    // ---- Directional-light gizmo ----
    // A directional light has no position or range that affects lighting, only
    // orientation, so the gizmo is the Unreal/Unity shape: a small disc facing
    // the beam with a bundle of parallel arrows shooting out of it, anchored at
    // the entity. Fixed world size for the same reason: nothing about the light
    // gives it a natural scale.
    template <class Emit>
    void DrawDirectionalGizmo(const Emit& emit, const XMFLOAT3& origin,
                              const XMFLOAT3& dir, uint32_t color)
    {
        constexpr float kDiscRadius = 0.45f;
        constexpr float kRayLength  = 2.2f;
        constexpr float kHeadLength = 0.35f;
        constexpr float kHeadFlare  = 0.13f;
        constexpr int   kRays       = 5;   // centre + 4 around the rim
        constexpr int   kDiscSeg    = 24;

        const XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&dir));
        const XMVECTOR o = XMLoadFloat3(&origin);
        const XMVECTOR u = XMVector3Normalize(XMVector3Cross(PerpSeed(d), d));
        const XMVECTOR v = XMVector3Cross(d, u);

        // Disc facing the beam: reads as the "sun plate" the rays leave from.
        XMFLOAT3 prev = StoreF3(XMVectorAdd(o, XMVectorScale(u, kDiscRadius)));
        for (int s = 1; s <= kDiscSeg; ++s)
        {
            const float a = (float(s) / kDiscSeg) * XM_2PI;
            const XMFLOAT3 p = StoreF3(XMVectorAdd(o,
                XMVectorAdd(XMVectorScale(u, std::cos(a) * kDiscRadius),
                            XMVectorScale(v, std::sin(a) * kDiscRadius))));
            emit(prev, p, color);
            prev = p;
        }

        // Parallel rays with arrowheads. The heads make the SIGN of the
        // direction unambiguous; a bare line looks the same both ways.
        for (int r = 0; r < kRays; ++r)
        {
            XMVECTOR offset = XMVectorZero();
            if (r > 0)
            {
                const float a = (float(r - 1) / (kRays - 1)) * XM_2PI;
                offset = XMVectorAdd(XMVectorScale(u, std::cos(a) * kDiscRadius),
                                     XMVectorScale(v, std::sin(a) * kDiscRadius));
            }
            const XMVECTOR start = XMVectorAdd(o, offset);
            const XMVECTOR tip   = XMVectorAdd(start, XMVectorScale(d, kRayLength));
            const XMFLOAT3 startF = StoreF3(start);
            const XMFLOAT3 tipF   = StoreF3(tip);
            emit(startF, tipF, color);

            const XMVECTOR back = XMVectorSubtract(tip, XMVectorScale(d, kHeadLength));
            for (int h = 0; h < 4; ++h)
            {
                const float a = (float(h) / 4.f) * XM_2PI;
                const XMVECTOR flare =
                    XMVectorAdd(XMVectorScale(u, std::cos(a) * kHeadFlare),
                                XMVectorScale(v, std::sin(a) * kHeadFlare));
                emit(tipF, StoreF3(XMVectorAdd(back, flare)), color);
            }
        }
    }

    // Point = wireframe sphere; spot = capped cone; directional = arrow bundle.
    void EmitLightRanges(World& world, DebugWirePass& dbg)
    {
        auto* pool = world.GetPool<LightData>();
        if (!pool) return;
        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        const XRayLine emit{ dbg };
        constexpr uint32_t kCol = 0xFFFFD040u;
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const Entity e = ents[i];
            if (!world.IsAlive(e)) continue;
            const LightData& L = data[i];
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;
            const XMFLOAT3 pos{ gt->matrix._41, gt->matrix._42, gt->matrix._43 };
            // Direction = entity forward (+Z), the SAME derivation the renderer
            // uses (LightUtil), so the gizmo can never disagree with the light.
            const XMFLOAT3 wdf = LightUtil::DirectionFromWorldF(gt->matrix);
            switch (L.type)
            {
            case LightType::Point:
                DrawSphere(emit, pos, L.radius, kCol);
                break;
            case LightType::Spot:
                DrawSpotGizmo(emit, pos, wdf, L.spotAngle, L.radius, kCol);
                break;
            case LightType::Directional:
                DrawDirectionalGizmo(emit, pos, wdf, kCol);
                break;
            }
        }
    }

    void EmitVolumeGizmos(World& world, DebugWirePass& dbg)
    {
        auto* pool = world.GetPool<ECS::PostProcessVolumeComponent>();
        if (!pool) return;
        constexpr uint32_t kBounds = 0xFF66CCFFu;  // light blue
        constexpr uint32_t kShell  = 0x8033AACCu;  // dimmer, translucent-ish

        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        const OverlayLine emit{ dbg };   // volumes stay see-through (always on top)
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const ECS::PostProcessVolumeComponent& v = data[i];
            if (v.isGlobal) continue;  // unbounded: nothing spatial to draw
            const Entity e = ents[i];
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;

            const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
            XMVECTOR scale, rotQ, trans;
            if (!XMMatrixDecompose(&scale, &rotQ, &trans, M)) continue;
            XMFLOAT3 half; XMStoreFloat3(&half, scale);

            if (v.shape == ECS::PPVolumeShape::Sphere)
            {
                XMFLOAT3 c; XMStoreFloat3(&c, trans);
                const float r = half.x;
                DrawSphere(emit, c, r, kBounds);
                if (v.blendDistance > 0.f) DrawSphere(emit, c, r + v.blendDistance, kShell);
            }
            else
            {
                DrawOBB(dbg, trans, rotQ, half, kBounds);
                if (v.blendDistance > 0.f)
                {
                    const XMFLOAT3 outer{ half.x + v.blendDistance,
                                          half.y + v.blendDistance,
                                          half.z + v.blendDistance };
                    DrawOBB(dbg, trans, rotQ, outer, kShell);
                }
            }
        }
    }

    // Local fog volumes: outer bounds + the inner shell where the edge fade
    // reaches full density. Sphere volumes are ellipsoids (radii = scale); the
    // wire sphere drawn is the largest radius, which contains it.
    void EmitFogVolumeGizmos(World& world, DebugWirePass& dbg)
    {
        auto* pool = world.GetPool<LocalFogVolumeComponent>();
        if (!pool) return;
        constexpr uint32_t kBounds   = 0xFFD9D9D9u;  // light grey (fog)
        constexpr uint32_t kCore     = 0x80A0A0A0u;  // dimmer inner shell
        constexpr uint32_t kDisabled = 0x80606060u;

        const auto& ents = pool->Entities();
        const auto& data = pool->Data();
        const OverlayLine emit{ dbg };
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const LocalFogVolumeComponent& v = data[i];
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(ents[i]);
            if (!gt) continue;
            XMVECTOR scale, rotQ, trans;
            if (!XMMatrixDecompose(&scale, &rotQ, &trans, XMLoadFloat4x4(&gt->matrix))) continue;
            XMFLOAT3 half; XMStoreFloat3(&half, XMVectorAbs(scale));
            const uint32_t col  = v.enabled ? kBounds : kDisabled;
            const float    core = 1.0f - std::clamp(v.edgeFade, 0.0f, 1.0f);

            if (v.shape == LocalFogShape::Sphere)
            {
                XMFLOAT3 c; XMStoreFloat3(&c, trans);
                const float r = (std::max)({ half.x, half.y, half.z });
                DrawSphere(emit, c, r, col);
                if (v.enabled && core > 0.02f && core < 0.98f)
                    DrawSphere(emit, c, r * core, kCore);
            }
            else
            {
                DrawOBB(dbg, trans, rotQ, half, col);
                if (v.enabled && core > 0.02f && core < 0.98f)
                    DrawOBB(dbg, trans, rotQ,
                            XMFLOAT3{ half.x * core, half.y * core, half.z * core }, kCore);
            }
        }
    }

    struct IconKind
    {
        DebugCategory category;
        const char*   name;          // Debug-menu label
        // Font Awesome codepoint (0 = use texturePath), baked once from the UI font
        // atlas into a bindless texture; no separate .itex needed.
        uint32_t      glyph;
        // .itex FALLBACK when the glyph is not available; resolved via
        // Renderer::GetDebugIconBindless.
        const char*   texturePath;
        void (*emit)(World&, DebugIconPass&, uint32_t /*texIdx*/, float /*halfSize*/);
        // A SCREEN FRACTION while DebugIconPass::constantScreenSize (the icon keeps
        // its on-screen size at any distance); else a fixed world half-extent.
        float         halfSize;
    };

    // The registry: one row per debug icon kind.
    // Icons are vector glyphs from the UI icon font (Font Awesome), baked once at
    // high resolution -> crisp at any gizmo size. texturePath is null (no .itex);
    // set it only if you add an icon kind with no matching font glyph.
    constexpr IconKind kIconKinds[] = {
        { DebugCategory::Lights,   "Light Icons",  0xF0EBu /*fa-lightbulb*/,
          nullptr, &EmitIconsForPool<LightData>,  0.080f },
        { DebugCategory::Cameras,  "Camera Icons", 0xF030u /*fa-camera*/,
          nullptr, &EmitCameraIcons,              0.085f },
        { DebugCategory::VFX,      "VFX Icons",    0xF06Du /*fa-fire*/,
          nullptr, &EmitVFXIcons,                 0.075f },
    };
    constexpr int kIconKindCount = static_cast<int>(sizeof(kIconKinds) / sizeof(kIconKinds[0]));
}

int DebugDrawSystem::IconKindCount() { return kIconKindCount; }

DebugDrawSystem::IconKindInfo DebugDrawSystem::GetIconKindInfo(int index)
{
    if (index < 0 || index >= kIconKindCount) return { "", DebugCategory::None };
    return { kIconKinds[index].name, kIconKinds[index].category };
}

uint32_t DebugDrawSystem::GetIconKindGlyph(int index)
{
    if (index < 0 || index >= kIconKindCount) return 0u;
    return kIconKinds[index].glyph;
}

// ==== Module wireframes ====
namespace
{
    std::vector<DebugDrawSystem::WireContributor>& WireContributors()
    {
        static std::vector<DebugDrawSystem::WireContributor> s_contributors;
        return s_contributors;
    }
}

void DebugDrawSystem::AddWireContributor(WireContributor contributor)
{
    WireContributors().push_back(std::move(contributor));
}

// ==== ApplyTo: runs BEFORE Renderer::BeginFrame (see the header) ====
void DebugDrawSystem::ApplyTo(Renderer& renderer) const
{
    if (DebugWirePass* dbg = renderer.GetDebugWirePass())
    {
        dbg->enabled              = masterEnabled;
        dbg->showAABBs            = Active(DebugCategory::AABB);
        dbg->showFrustum          = Active(DebugCategory::Frustum);
        dbg->showCapsules         = Active(DebugCategory::Capsules);
        dbg->showReflectionProbes = Active(DebugCategory::ReflectionProbes);
        dbg->showDDGIVolumes      = Active(DebugCategory::DDGIVolumes);
        dbg->showCollision        = Active(DebugCategory::Collision);
        dbg->showPhysics          = Active(DebugCategory::Physics);
        dbg->showDecals           = Active(DebugCategory::Decals);
        dbg->collisionMaxDistance = collisionMaxDistance;
    }
    // Light icons go to the dedicated DebugIconPass in Submit(), not through the
    // gameplay billboard path.
}

// ==== Submit: runs AFTER Renderer::BeginFrame and BEFORE Renderer::Render ====
void DebugDrawSystem::Submit(Renderer& renderer, World& world, const FrameContext& ctx,
                             DX12Physics::PhysicsSystem& physics) const
{
    // ---- Debug icons -> DebugIconPass (per category bit, independent of the wireframe master) ----
    // Cleared every frame, so toggling off (or a Game build, which never calls
    // Submit) leaves the queue empty. Driven entirely by the kIconKinds[] registry.
    if (DebugIconPass* icons = renderer.GetDebugIconPass())
    {
        icons->ClearIcons();
        icons->maxDistance = iconMaxDistance;
        s_viewCameraEntity = static_cast<Entity>(ctx.cameraEntity);
        for (const IconKind& kind : kIconKinds)
        {
            if (!iconsEnabled || Sys::CommandLine::Get().Has("-no_debug_icons")) break;
            if (!DebugCatEnabled(categoryMask, kind.category)) continue;
            // Prefer the glyph-baked icon (font-atlas texture); fall back to the
            // .itex only if the glyph wasn't registered (no icon font).
            uint32_t texIdx = kind.glyph ? renderer.GetGlyphIconBindless(kind.glyph)
                                         : 0xFFFFFFFFu;
            if (texIdx == 0xFFFFFFFFu && kind.texturePath)
                texIdx = renderer.GetDebugIconBindless(kind.texturePath);
            if (texIdx == 0xFFFFFFFFu) continue;   // not ready yet (baked next frame)
            kind.emit(world, *icons, texIdx, kind.halfSize * iconSizeScale);
        }
    }

    // ---- Wireframe overlays (gated by the master switch) ----
    if (!masterEnabled) return;
    DebugWirePass* dbg = renderer.GetDebugWirePass();
    if (!dbg) return;

    // The modules' (the Nav module's navmesh and soup). They share the Collision overlay's
    // distance dial: per-collider geometry can flood the wire buffer on a city-sized level.
    if (!WireContributors().empty())
    {
        DirectX::XMFLOAT3 camPos{ 0, 0, 0 };
        if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(static_cast<Entity>(ctx.cameraEntity)))
            camPos = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
        const WireContext wireCtx{ *dbg, world, camPos, collisionMaxDistance };
        for (const WireContributor& c : WireContributors())
            if (Active(c.category)) c.emit(wireCtx);
    }

    if (Active(DebugCategory::LightRanges))
        EmitLightRanges(world, *dbg);

    if (Active(DebugCategory::PostProcessVolumes))
        EmitVolumeGizmos(world, *dbg);

    if (Active(DebugCategory::FogVolumes))
        EmitFogVolumeGizmos(world, *dbg);

    if (Active(DebugCategory::Collision))
    {
        // `collisionMaxDistance <= 0` = unlimited; the Debug menu dials it down.
        const Entity camEnt = static_cast<Entity>(ctx.cameraEntity);
        if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(camEnt))
        {
            const DirectX::XMFLOAT3 camPos{
                gt->matrix._41, gt->matrix._42, gt->matrix._43 };
            Tools::CollisionMesh::EmitDebugWireframe(
                world, camPos, collisionMaxDistance, *dbg, physics,
                Tools::CollisionMesh::kWireStatic, static_cast<Entity>(selectedEntity));
        }
    }
}
