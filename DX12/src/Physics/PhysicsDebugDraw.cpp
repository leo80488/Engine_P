// PhysicsDebugDraw: Jolt's DebugRendererSimple drawing into the debug wire pass.

#include "Physics/PhysicsDebugDraw.h"

#ifdef JPH_DEBUG_RENDERER

#include "RenderGraph/RenderPass/DebugWirePass.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Renderer/DebugRendererSimple.h>

namespace DX12Physics
{
namespace
{
    // The lines go straight into the wire pass; the solid triangles Jolt may ask for are never
    // requested (the draw is wireframe), and there is no text.
    class WireDebugRenderer final : public JPH::DebugRendererSimple
    {
    public:
        void SetTarget(DebugWirePass* wire) { m_wire = wire; }

        void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override
        {
            if (!m_wire) return;
            m_wire->AddLine({ from.GetX(), from.GetY(), from.GetZ() }, { to.GetX(), to.GetY(), to.GetZ() },
                            color.GetUInt32());
        }

        void DrawText3D(JPH::RVec3Arg /*position*/, const std::string_view& /*text*/, JPH::ColorArg /*color*/,
                        float /*height*/) override
        {
        }

    private:
        DebugWirePass* m_wire = nullptr;
    };

    // Counts the bodies Jolt walks while drawing, so the caller can check it saw all of them.
    class CountingFilter final : public JPH::BodyDrawFilter
    {
    public:
        bool ShouldDraw(const JPH::Body& body) const override
        {
            ++m_drawn;
            if (body.IsDynamic() && !body.IsActive()) ++m_asleep;
            return true;
        }

        std::uint32_t Drawn() const { return m_drawn; }
        std::uint32_t Asleep() const { return m_asleep; }

    private:
        mutable std::uint32_t m_drawn  = 0;
        mutable std::uint32_t m_asleep = 0;
    };
}

struct PhysicsDebugDraw::Impl
{
    WireDebugRenderer renderer;
};

PhysicsDebugDraw::PhysicsDebugDraw() : m_impl(std::make_unique<Impl>()) {}

PhysicsDebugDraw::~PhysicsDebugDraw() = default;

DebugDrawStats PhysicsDebugDraw::Draw(JPH::PhysicsSystem& physics,
                                      const std::vector<const JPH::CharacterVirtual*>& characters,
                                      DebugWirePass& wire, const DirectX::XMFLOAT3& cameraPos)
{
    WireDebugRenderer& renderer = m_impl->renderer;
    renderer.SetTarget(&wire);
    renderer.SetCameraPos(JPH::RVec3(cameraPos.x, cameraPos.y, cameraPos.z));

    // Static grey, awake dynamic and kinematic yellow / green, asleep red; sensors yellow.
    JPH::BodyManager::DrawSettings settings;
    settings.mDrawShape          = true;
    settings.mDrawShapeWireframe = true;
    settings.mDrawShapeColor     = JPH::BodyManager::EShapeColor::SleepColor;
    CountingFilter filter;
    physics.DrawBodies(settings, &renderer, &filter);

    // A character controller owns no body in the world (unless queryBody adds a stand-in), so its capsule
    // is drawn from its shape: green on the ground, cyan in the air.
    DebugDrawStats stats;
    for (const JPH::CharacterVirtual* character : characters)
    {
        const bool grounded = character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
        character->GetShape()->Draw(&renderer, character->GetCenterOfMassTransform(), JPH::Vec3::sReplicate(1.0f),
                                    grounded ? JPH::Color::sGreen : JPH::Color::sCyan, false, true);
        ++stats.characters;
    }

    stats.bodies = filter.Drawn();
    stats.asleep = filter.Asleep();
    renderer.SetTarget(nullptr);
    return stats;
}

// PhysicsSystem's entry point lives here, not in PhysicsSystem.cpp: whatever links the physics core
// (both exes, the standalone unit tests) then pulls this TU, Jolt's debug renderer and DebugWirePass
// only when something draws.
DebugDrawStats PhysicsSystem::DrawDebug(DebugWirePass& wire, const DirectX::XMFLOAT3& cameraPos)
{
    DebugTarget target = GetDebugTarget();
    if (!target.physics || !target.renderer) return {};
    if (!*target.renderer) *target.renderer = std::make_shared<PhysicsDebugDraw>();
    return (*target.renderer)->Draw(*target.physics, target.characters, wire, cameraPos);
}
}

#endif
