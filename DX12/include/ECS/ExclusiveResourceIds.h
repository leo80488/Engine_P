#pragma once

// ExclusiveResourceIds - stable ids for NON-COMPONENT shared state, used with
// SystemAccessBuilder::ExclusiveResource() in ISystem::DeclareAccess.
// The Scheduler never co-batches two systems that declare the same id, even with
// disjoint component sets. Use them for singletons whose internals are not
// thread-safe (or whose thread safety should not be relied on): the Jolt physics
// world, the audio voice pool, the Lua VM, the EventBus subscriber lists, the
// Renderer's per-frame upload rings.
// Ids are arbitrary but MUST stay unique: add new ones at the end and never reuse a
// value. A shared id serializes two unrelated systems (harmless); a split one lets
// two systems that share state run together (heap corruption under load).

#include <cstdint>

namespace ExclusiveRes
{
    inline constexpr uint64_t kPhysicsWorld   = 1; // DX12Physics::PhysicsSystem + Jolt bodies/queries
    inline constexpr uint64_t kAudioEngine    = 2; // AudioEngine + AudioSystem/Audio3DSystem voice state
    inline constexpr uint64_t kEventBus       = 3; // EventBus::Get() publish/subscribe/dispatch
    inline constexpr uint64_t kScriptVM       = 4; // sol::state - the single Lua VM (ScriptSystem, AI BT)
    inline constexpr uint64_t kRenderer       = 5; // Renderer + its upload rings / GPU-visible state
    inline constexpr uint64_t kAnimClipSystem = 6; // AnimClipSystem clip acquire/sample cache
    inline constexpr uint64_t kCameraStack    = 7; // camera-stack shared blend/priority state

    // kScriptVM, kRenderer and kCameraStack are declared by nobody, and correctly so:
    // every system that runs Lua (ScriptTimer, ScriptLogic, GameFlowTick, AIBT,
    // VFXSpawn) or calls into the Renderer (the Animation-phase chain, Video,
    // Sequencer) is OPAQUE, which already serializes it against everything. kScriptVM /
    // kRenderer become live when one of them turns non-opaque (a system declaring
    // kScriptVM gets the Scheduler's Lua VM grant on its worker; Scripting/ScriptVM.h).
    // kCameraStack is superseded by the CameraStackChannelSingleton COMPONENT, which the
    // camera systems declare as a Write: keep the id reserved, do not reuse it.
    inline constexpr uint64_t kPostFxRuntime  = 8; // PostProcess::Runtime::Get() resolved settings + override stack

    // Reserved and undeclared by design: GuidRegistry::Get(). AttachmentRef::Resolve
    // does a read-only Find() on the registry map, safe concurrently because every
    // registry MUTATOR runs on a serial path (entity-destroy listeners fire in the
    // phase-boundary CommandBuffer flush; EnsureGuidOn / BindAndStamp run from the
    // opaque ScriptSystem, scene load or the editor's main-thread UI). If a declared
    // parallel system ever mutates the registry in Update, give it AND every
    // Resolve caller this id.
    inline constexpr uint64_t kGuidRegistry   = 9;
}
