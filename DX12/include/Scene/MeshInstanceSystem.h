#pragma once
// MeshInstanceSystem: derives a StaticMeshComponent owner's section entities.
//
// Phase GameplayPostLogic, like SequencerTickSystem and for the same reasons: it creates and
// destroys entities directly, which only a serial phase allows (a static_assert in the .cpp
// pins the phase), and it runs before PhysicsInterpolation, so a section spawned this frame
// gets its GlobalTransform and WorldAabb from TransformPropagate before the renderer gathers it.
// Run mask Always: an owner placed, edited or undone in Edit mode must show at once; sections
// are derived data, not simulation.
//
// Every frame, for every owner: are its sections the ones its current authored fields would
// give (the signature each section remembers, their count)? If not - an Inspector edit,
// Entity.set, an undo, a deserialize, a section deleted by hand - they are destroyed and
// derived again. Sections whose owner is gone (deleted, or its component removed) are
// destroyed. That check, not a dirty flag, is the contract, so no writer of
// StaticMeshComponent has to notify anyone.
//
// -staticmesh_no_derive turns derivation off (the reverse arm: an owner then draws nothing).

#include "ECS/ECS.h"
#include "ECS/ISystem.h"

class Renderer;
struct FrameContext;

class MeshInstanceSystem final : public SystemInPhase<TickPhase::GameplayPostLogic>
{
public:
    // @p renderer may be null (headless): the system then does nothing.
    explicit MeshInstanceSystem(Renderer* renderer);

    const char* GetName() const override { return "MeshInstanceSystem"; }
    SystemRunMask RunMask() const override { return SystemRunMask::Always; }
    // Opaque: it creates entities and adds components of a dozen types. The
    // phase is serial anyway.
    void DeclareAccess(SystemAccessBuilder& b) const override { b.OpaqueAccess(); }
    void Update(World& world, const FrameContext& ctx) override;

    // Derive @p owner's sections now (what Update does when they are stale).
    // Returns how many were spawned; 0 when the mesh does not resolve (the
    // owner's StaticMeshComponent::status then says why).
    static unsigned Derive(World& world, Entity owner, Renderer& renderer);

    // Destroy every derived section of @p owner.
    static void DestroySections(World& world, Entity owner);

private:
    Renderer* m_renderer = nullptr;
    bool      m_disabled = false;
};
