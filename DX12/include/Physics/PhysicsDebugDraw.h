#pragma once

// PhysicsDebugDraw - Jolt's debug renderer wired to the engine's debug wire pass.
// Compiled where Jolt has a debug renderer (JPH_DEBUG_RENDERER: every configuration but Retail).

#ifdef JPH_DEBUG_RENDERER

#include "Physics/PhysicsSystem.h"

#include <memory>
#include <vector>

namespace JPH
{
    class CharacterVirtual;
    class PhysicsSystem;
}

namespace DX12Physics
{
    class PhysicsDebugDraw
    {
    public:
        PhysicsDebugDraw();
        ~PhysicsDebugDraw();

        PhysicsDebugDraw(const PhysicsDebugDraw&)            = delete;
        PhysicsDebugDraw& operator=(const PhysicsDebugDraw&) = delete;

        // Draws the bodies of @p physics and the capsules of @p characters into @p wire, wireframe,
        // coloured by sleep state; @p cameraPos picks each shape's level of detail.
        DebugDrawStats Draw(JPH::PhysicsSystem& physics, const std::vector<const JPH::CharacterVirtual*>& characters,
                            DebugWirePass& wire, const DirectX::XMFLOAT3& cameraPos);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}

#endif
