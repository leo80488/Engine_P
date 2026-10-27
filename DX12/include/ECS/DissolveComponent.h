#pragma once

// DissolveComponent - per-entity noise dissolve (burn-out / materialize) on any mesh, with no
// material setup. Renderer::BuildScene_ComputeDissolveSet resolves the tagged subtrees each frame
// (a tag on a root dissolves every child mesh part; nested tags override) into GPUInstanceData;
// the material surface template and Shadow.ps clip against the engine-global noise texture and
// add the edge glow to emissive.
// Lua sets intent only (Engine.Dissolve / DissolveIn / SetDissolveAmount); DissolveSystem animates
// `amount` toward `target` in C++.
// Only the authored look fields are serialized: a scene saved mid-dissolve loads intact (amount 0).

#include "ECS/ECS.h"
#include "ECS/ISystem.h"

#include <DirectXMath.h>

struct DissolveComponent
{
    // ---- Authored look (serialized) ----
    DirectX::XMFLOAT3 edgeColor = { 1.0f, 0.45f, 0.1f };
    float edgeWidth     = 0.08f;  // glow band width in noise units (0..~0.3)
    float edgeIntensity = 6.0f;   // HDR emissive multiplier on the edge band
    float noiseScale    = 1.0f;   // UV tiling of the global noise texture

    // ---- Runtime state (NOT serialized) ----
    float amount   = 0.0f;   // 0 = intact ... 1 = fully dissolved (renderer reads it live)
    float target   = 0.0f;   // amount moves toward this each tick
    float duration = 0.0f;   // seconds for a full 0 -> 1 sweep; <= 0 snaps
    bool  destroyOnComplete = false; // DestroyEntity once a dissolve-out finishes
};

// Moves DissolveComponent.amount toward .target. Runs in GameplayPreLogic (next to
// LifetimeSystem) so the value is final before the Render phase reads it. Play only: in Edit
// mode the inspector's `amount` slider still previews, because the renderer reads the component
// every frame.
class DissolveSystem final : public SystemInPhase<TickPhase::GameplayPreLogic>
{
public:
    const char* GetName() const override { return "DissolveSystem"; }
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    void OnRegister(World& world) override;
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
};
