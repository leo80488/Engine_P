#pragma once

// WaterRippleSystem - interactive ripples for the multi-body water surface.
//
// Each ripple-enabled water body - a WaterComponent, or a Water-shading mesh with a
// WaterInteractionComponent - gets a fixed-resolution GPU height field (a ping-pong pair of
// R16G16_FLOAT textures: .r = h(t), .g = h(t-dt)). Every frame one compute dispatch
// (WaterRippleStep.cs.hlsl) advances an explicit 2D wave equation and injects the frame's
// impulses; the water shading then samples the field and bends the surface normal by its gradient
// (normal-only ripples, no vertex displacement).
//
// Impulse sources:
//   - Physics: Sync() scans dynamic rigid bodies each frame; a body crossing a water surface
//     splashes (impulse ~ vertical speed), one skimming just under/over it leaves a wake
//     (impulse ~ horizontal speed). Water has no collider, so this position scan - not
//     ContactBeganEvent - is the trigger.
//   - Script: Engine.RippleAt(x, z, radius, strength) queues a world-space impulse for the next
//     Sync.
//
// Lifecycle (owned by Renderer):
//   Init(gfx)                           - compile the step PSO, allocate the slot pool.
//   Sync(world, dt)  [before SyncWater] - pick active bodies, build impulse lists, publish each
//                                         field (WaterComponent::rippleFieldSRV or
//                                         WaterInteractionComponent::rippleBindlessIndex).
//   Execute(cl)      [render phase 0.6] - record the wave-step compute + UAV -> SRV transition so
//                                         the water pass can sample it.
//   Shutdown()                          - free the slot textures.

#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "ECS/ECS.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

class IGraphicsDevice;
class World;

// CB mirror for WaterRippleStep.cs.hlsl (b0 space2). 256-aligned so the per-body entries are
// independent CBV slots in the ring.
inline constexpr uint32_t kMaxRipplePoints = 32;
struct alignas(256) RippleStepCB
{
    uint32_t          resolution;
    float             c2;
    float             damping;
    uint32_t          pointCount;
    uint32_t          clear;        // 1 -> zero the field (slot reclaim)
    int32_t           shiftX;       // texel re-anchor when the sim window moves
    int32_t           shiftZ;
    float             _pad0;
    DirectX::XMFLOAT4 points[kMaxRipplePoints];   // xy=uv(window), z=radius(uv), w=impulse
};

class WaterRippleSystem
{
public:
    // Fixed field resolution for every body, so the pool is allocated once. Also WaterPass's
    // gradient stencil step (cb.rippleTexel = 1 / kResolution). 512 gives fine enough wavelets that
    // ripples read as ripples, not blobs, even on larger bodies.
    static constexpr uint32_t kResolution = 512;

    // How many bodies can own a ripple field at once. Deliberately smaller than
    // WaterPass::kMaxWaterBodies (16 drawn bodies): a render slot is one 256 B CBV in a per-frame
    // ring, a ripple slot is a ping-pong pair of kResolution^2 R16G16_FLOAT textures (~2 MiB; ~16 MiB
    // for the pool) allocated in Init and never freed. Do not make the two equal: bodies past this cap
    // render flat and Sync() logs once. kMaxBodies > kMaxWaterBodies would be the bug - a field
    // simulated every frame for a body the water pass never draws.
    static constexpr uint32_t kMaxBodies  = 8;

    struct ActiveBody
    {
        float    winOriginX = 0.0f, winOriginZ = 0.0f, winSize = 1.0f;
        float    bodyOriginX = 0.0f, bodyOriginZ = 0.0f, bodySize = 1.0f, level = 0.0f;
        uint32_t bindlessIndex = 0;   // 0 = no field this frame
    };

    void Init(IGraphicsDevice& gfx);
    void Shutdown();
    // Hot reload (called by Renderer::ReloadShaders).
    void ReloadShaders(IGraphicsDevice& gfx);

    // Call before BuildScene_SyncWater: chooses active bodies, places each body's sim window
    // (follows the latest interaction, else the camera at @p camX / @p camZ), builds the per-body
    // impulse lists (physics scan + queued script impulses), and publishes each body's field + window
    // footprint (WaterComponent::rippleFieldSRV, WaterInteractionComponent::rippleBindlessIndex, and
    // the ActiveBody list).
    void Sync(World& world, float dt, float camX, float camZ);

    // Renderer phase 0.6 (own command list, before the render graph): records the wave-step compute
    // for every active body and transitions the result to a shader resource for the in-graph water
    // pass.
    void Execute(RHI::CommandList cl);

    // Queues a world-space ripple (Lua Engine.RippleAt / gameplay), consumed by the next Sync();
    // ignored if it falls outside every ripple-enabled body.
    void AddRipple(float worldX, float worldZ, float radius, float strength);

    // Must be called from Renderer::OnWorldClear. Slots are keyed by Entity and entity ids are
    // recycled across worlds: without this a new entity reusing an old id inherits the previous
    // world's height field, and the first frame replays stale m_prevBodyPos as phantom splashes.
    void OnWorldClear();

    // One live ripple body, published by the Renderer into LightCB so water_common.hlsli's
    // WaterRippleSlope() finds it by world-XZ containment. bindlessIndex is RHI::Texture::handleId -
    // an index into the bindless heap, not a descriptor handle (never use Slot::srv here).
    const std::vector<ActiveBody>& GetActiveBodies() const { return m_activeBodies; }

    static constexpr uint32_t GetResolution() { return kResolution; }

private:
    struct Slot
    {
        RHI::Texture       tex[2];
        uint64_t           uav[2]  = { 0, 0 };
        uint64_t           srv[2]  = { 0, 0 };
        RHI::ResourceState state[2] = { RHI::ResourceState::UNORDERED_ACCESS,
                                        RHI::ResourceState::UNORDERED_ACCESS };
        uint32_t           read      = 0;          // which tex holds the current field
        Entity             owner     = NullEntity; // body using this slot (NullEntity = free)
        bool               valid     = false;
        bool               needsClear = true;      // zero both fields before first use
        bool               winInit   = false;      // has the sim window been placed yet

        // Water body footprint (source containment + surface level).
        float waterOriginX = 0, waterOriginZ = 0, waterSize = 1, level = 0;
        // Sim window - the high-detail field's world footprint (follows the action).
        float areaSize    = 120.0f;
        float winOriginX  = 0, winOriginZ = 0;     // window world origin
        float winCenterX  = 0, winCenterZ = 0;     // snapped window centre (shift basis)
        int   shiftX      = 0, shiftZ     = 0;      // texel shift applied this frame

        float c2 = 0.15f, damping = 0.03f, amplitude = 1.0f;
        std::vector<DirectX::XMFLOAT4> raw;        // this-frame raw world sources (x,z,radius,impulse)
        std::vector<DirectX::XMFLOAT4> points;     // window-uv impulses for the CB
    };

    struct QueuedRipple { float x, z, radius, strength; };

    using CBFrame = std::array<RippleStepCB, kMaxBodies>;

    void CreatePipelines(IGraphicsDevice& gfx);

    int  SlotForEntity(Entity e);                  // existing or a free slot, else -1
    void GatherWorldImpulse(float wx, float wz, float radius, float strength);
    void ScanDynamicBodies(World& world, float dt);
    void PlaceWindowAndBuildPoints(Slot& s, float camX, float camZ);
    static void AddRawSource(Slot& s, float wx, float wz, float worldRadius, float impulse);

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_stepPSO;

    FrameCB<CBFrame>   m_cb;

    std::array<Slot, kMaxBodies>     m_slots;
    std::vector<int>                 m_active;      // slot indices live this frame
    std::vector<QueuedRipple>        m_queue;       // script/gameplay impulses
    std::vector<ActiveBody>          m_activeBodies; // published to LightCB each Sync
    std::unordered_map<Entity, DirectX::XMFLOAT3> m_prevBodyPos;  // surface-cross tracking
};
