#pragma once

// AnimProfile - one frame's animation-chain cost, stage by stage.
//
// Tracy zones the same work, but answering "which stage of Renderer::TickAnimationChain
// owns the cost" with Tracy needs a capture per experiment and is impossible in an
// uncaptured Profile/Release run; this is a few floats written once per frame.
// Stages mirror TickAnimationChain in order (a waterfall); TotalMs() is the
// "CPU animation frame time".
//
// Not thread-safe by design: written from the Animation phase on the main thread
// (parallel sections publish their totals afterwards), read between frames.

#include <chrono>
#include <cstdint>

namespace Anim
{
    struct AnimProfile
    {
        // ---- Per-stage wall time (milliseconds), in chain order ----
        float visibilityPrepassMs = 0.f; // outside Animation phase; includes cullMs when enabled
        float cullMs        = 0.f;   // AnimVisibilityCull
        float sampleMs      = 0.f;   // AnimationSystem::Update - clip sample + blend
        float ikRigMs       = 0.f;
        float ikRigPrepareMs= 0.f, ikRigSolveMs=0.f;
        uint32_t ikRigEntities=0,ikRigProbes=0,ikRigSolves=0;
        float ikMs          = 0.f;
        float chainMs       = 0.f;   // ChainPhysicsSystem
        float localToWorldMs= 0.f;   // LocalToWorldSystem - pose -> skinning matrices
        float socketMs      = 0.f;
        float followMs      = 0.f;
        float faceMs        = 0.f;   // FaceShading
        float headTrackMs   = 0.f;
        // FacialAnimationSystem (morph targets / blend shapes) - not faceMs, which is
        // FaceShading (SDF face shadows).
        float facialMs      = 0.f;
        float tailMs        = 0.f;   // decal lifetimes + EventBus::DispatchAll
        float prologueMs    = 0.f;   // descriptor-heap + 4 ring BeginFrame calls
        float boneAabbMs    = 0.f;   // per-skeleton bone-AABB merge (parallel)
        float skinMatrixMs  = 0.f;   // SkinMatrixSystem - pose -> skinning matrices
        float skinJobsMs    = 0.f;   // BuildSkinJobs - GPU dispatch prep + ring alloc
        // PoseRingBuffer::FlushToGPU - the one sequential shadow->UPLOAD copy.
        float poseFlushMs   = 0.f;

        // The whole of Renderer::TickAnimationChain (the profiler's RendererAnimationChainSystem).
        // TotalMs() only sums the stages, so `wallMs + visibilityPrepassMs - TotalMs()` is
        // unattributed work - keep both, or a stage that was never wrapped reads as free.
        float wallMs        = 0.f;

        // The four phases the profiler's "Animation" zone spans: App::Run wraps
        // TickPhase::Animation + BoneAttachment + SecondaryPhysics + PreRender in one
        // ZoneScopedN("Animation"), while wallMs is only RendererAnimationChainSystem, one
        // system inside the first of them.
        float phaseAnimationMs       = 0.f;
        float phaseBoneAttachmentMs  = 0.f;
        float phaseSecondaryPhysMs   = 0.f;
        float phasePreRenderMs       = 0.f;
        float phaseAllFourMs         = 0.f;   // == the profiler's "Animation" zone
        // The other system in TickPhase::Animation.
        float animDriverMs           = 0.f;
        // The third system in TickPhase::Animation (declared in TimelineSystem.h, not
        // EngineSystems.h).
        float timelineMs             = 0.f;

        // Summed per-agent work across every thread, for the stages that fan out: cpu/wall
        // is the parallel speedup achieved, cpu - wall is what left the main thread.
        float sampleCpuMs   = 0.f;
        float localToWorldCpuMs = 0.f;
        // Same split for the chain. Serial = the prologue (FK cache, group tables, job
        // build, cost sort); parallel = the island ParallelFor, in wall and in summed core
        // time.
        float chainSerialMs   = 0.f;
        float chainParallelMs = 0.f;   // wall
        float chainCpuMs      = 0.f;   // summed across every worker
        // Where that CPU goes, main lane only (assigned per pass, like the three above):
        // summed solve core time per LOD tier, the share spent on shadow-only entities
        // (admitted by a shadow cascade, not the camera - the LOD policy's biggest lever),
        // and the pipeline's FK + prepare jobs, which chainCpuMs excludes.
        float chainTierCpuMs[3]    = { 0.f, 0.f, 0.f };
        float chainShadowOnlyCpuMs = 0.f;
        float chainPrepCpuMs       = 0.f;
        // The same three for the editor's Chain Physics Preview lane. The fields above are
        // assigned once per pass and never cleared, so a second pass would overwrite the
        // scene's numbers: ChainPhysicsSystem::RunPass writes chain*Ms for Lane::Main only
        // and these for Lane::Preview only. Stale between preview sessions.
        float previewChainSerialMs   = 0.f;
        float previewChainParallelMs = 0.f;
        float previewChainCpuMs      = 0.f;

        // ---- Population ----
        std::uint32_t skeletons     = 0;   // SkeletonComponent pool size
        std::uint32_t animated      = 0;   // passed the visibility cull
        // Why each skeleton was admitted: without it "animated == skeletons" cannot tell
        // "cull off", "all on screen" and "AABBs missing, cull admits all" apart.
        std::uint32_t cullNoAabb    = 0;   // no WorldAabb -> admitted blind
        std::uint32_t cullFrustum   = 0;   // in the camera frustum
        std::uint32_t cullShadow    = 0;   // out of frustum, but casts into a cascade
        std::uint32_t cullSkipped   = 0;   // actually culled
        // Which cascade admitted each shadow-only skeleton (sums to cullShadow): a skeleton
        // shadowing cascade 0 must keep animating, one that only clips the 2 km terrain
        // cascade is waste.
        std::uint32_t cullShadowCascade[4] = { 0, 0, 0, 0 };
        // Set while the dev-only shadow holdout forces shadow-only skeletons to cull.
        // Rendering is wrong in this mode (T-posed shadow casters); the flag marks any
        // measurement taken under it as not shippable.
        bool shadowHoldout = false;
        std::uint32_t bonesSampled  = 0;   // summed over animated skeletons
        std::uint32_t skinnedVerts  = 0;   // this frame's ring consumption
        std::uint32_t skinSections  = 0;

        // Facial (morph-target) layer. facialEntities = what FacialAnimationSystem resolved;
        // facialSlots = what BuildSkinJobs wrote into the morph pool - the only observable
        // downstream of the component, so facial regressions are asserted against it (a
        // frozen face changes no drawPackets, engineErrors or luma).
        std::uint32_t facialEntities = 0;
        std::uint32_t facialSlots    = 0;
        // Skin jobs with any morph weight above kMorphActiveEpsilon, and the count of such
        // weights - what SkinningPass uploads and Skin.cs walks. Both 0 = no morph work.
        std::uint32_t morphJobs      = 0;
        std::uint32_t morphActive    = 0;

        // Chain-physics solver load. chainSubsteps / chainEntities is the mean substep
        // count; the solver runs nSub * (1 + iterations) + 1 collision passes per island
        // task, so a mean pinned at the cap of 8 costs ~65 passes instead of ~9.
        // Counts entities, not islands (incremented in the per-entity serial phase): one
        // Rossi is 13 islands / 11 tasks, so the ParallelFor width is ~11x this number.
        std::uint32_t chainEntities = 0;
        std::uint32_t chainTier0 = 0, chainTier1 = 0, chainTier2 = 0;
        std::uint32_t chainShadowOnly = 0;   // of chainEntities: shadow-only (main lane)
        std::uint32_t chainSubsteps = 0;
        // The Chain Physics Preview lane's share of the five above, accumulated by RunPass
        // for Lane::Preview only and cleared beside them in TickAnimationChain. The five
        // above sum both lanes; these feed the preview panel's stats overlay (summed over
        // the frame's preview passes: a 2-step frame counts the stage twice).
        std::uint32_t previewChainEntities = 0;
        std::uint32_t previewChainTier0 = 0, previewChainTier1 = 0, previewChainTier2 = 0;
        std::uint32_t previewChainSubsteps = 0;

        float TotalMs() const
        {
            return cullMs + sampleMs + ikRigMs + ikMs + chainMs
                 + localToWorldMs + socketMs + followMs + faceMs
                 + headTrackMs + facialMs + tailMs + prologueMs
                 + boneAabbMs + skinMatrixMs + skinJobsMs + poseFlushMs;
        }

        // Process-global sink: a singleton rather than a member threaded through many call
        // sites, because it is a measurement side channel nothing decides on.
        static AnimProfile& Get()
        {
            static AnimProfile s_instance;
            return s_instance;
        }
    };

    // Millisecond stopwatch. steady_clock, not high_resolution_clock: the latter aliases
    // system_clock in this toolchain and is not monotonic, so a clock correction would
    // produce a negative sample.
    struct StageTimer
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        float Ms() const
        {
            return std::chrono::duration<float, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
        }
    };
}
