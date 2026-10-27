#pragma once

// FaceTrackingSystem - drives characters from FaceTracking::Service's sources.
// Two calls in Renderer::TickAnimationChain, main thread, serial:
//   1. AnimationSystem::Update           - clips; restores last frame's journaled bones first
//   2. FaceTrackingSystem::Resolve       - THIS: Service::Pump, then per binding: the new
//                                          frames (calibrate, mirror, filter), the authority,
//                                          the morph rules -> FacialAnimationComponent::tracked,
//                                          HeadTrack's handoff weight, the bone targets
//   3. FacialAnimationSystem::Update     - folds the Tracked layer (after Authored)
//   4. IK / post-IK grants / HeadTrack   - HeadTrack's weight x (1 - headAuthority)
//   5. FaceTrackingSystem::ApplyPose     - THIS: the head chain, eye and jaw deltas onto the
//                                          local pose, each bone journaled first
//                                          (AnimationSystem::JournalBoneRotation)
//   6. ChainPhysics / LocalToWorld       - hair follows the head; the turn reaches the GPU
// Resolve and ApplyPose share one class, so the bone cache and the targets Resolve
// computed never leave it.
// PERFORMS NO STRUCTURAL WORLD MUTATION (like FacialAnimationSystem): FaceTracking.Attach
// adds the components; a binding whose carrier lacks a FacialAnimationComponent drives
// bones only. Per-binding state lives on FaceTrackingComponent::rt; nothing here is
// keyed by entity, so OnWorldClear needs no hook.

#include "ECS/ECS.h"
#include "ECS/AnimationSystem.h"
#include "FaceTracking/FaceFrame.h"
#include "Resource/SkeletonAsset.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace FaceTracking { struct FaceTake; struct FaceMapProfile; }
namespace Resource { class AnimationResource; }

class FaceTrackingSystem
{
public:
    // ---- Bake ----
    struct BakeOptions
    {
        float fps = 30.0f;     // the clips' key rate
        bool mirror = false;   // the performer as recorded, not their mirror image
        std::string clipName;  // both clips' name ("" = "FaceTake")
        std::string calibration; // the source whose calibration to apply ("" = none)
    };
    struct BakeReport
    {
        uint32_t keys = 0;
        float duration = 0.0f;
        uint64_t framesRead = 0;
        size_t morphs = 0, bones = 0;
        int folded = 0;        // chain bones above the root whose share is folded into its key
        std::string root;      // the bone the keyed bones hang from: the playing layer's CustomRoot
        std::string profile;   // the mapping used
        std::vector<std::string> warnings;
    };
    static constexpr uint32_t kMaxBakeKeys = 200000;

    FaceTrackingSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    //   dt        - the chain's animation delta (0 in Edit / paused: takes pause, and the
    //               authority snaps instead of fading - HeadTrack's rule)
    //   activeSet - when non-null, only entities in the anim-visible set evaluate their morphs
    //               and bones; every binding still consumes its frames
    void Resolve(World& world, float dt, const std::unordered_set<Entity>* activeSet = nullptr);
    void ApplyPose(World& world, const std::unordered_set<Entity>* activeSet = nullptr);

    // A .ifacemap changed on disk (hot reload): every binding recompiles on its next Resolve.
    static void InvalidateProfiles();
    static uint64_t ProfileGeneration();

    // A .ifacemap with its basePreset chain merged in (what a binding compiles). The
    // automatic mapping starts from the built-in one this way.
    static bool LoadProfileFile(const std::string& path, FaceTracking::FaceMapProfile& out, std::string& error);

    // The built-in mapping a profile-less binding uses on this skeleton (engine/facemaps/):
    // vroid for Fcl_* morphs, mmd_jp for PMX's Japanese morphs, arkit otherwise.
    static std::string AutoProfilePath(const SkeletonAsset& skel);

    // Editor A/B toggle: when false, Resolve clears every tracked list and authority and
    // ApplyPose writes nothing.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // Runs `take` through `character`'s mapping - the compile, calibration, filters and authority
    // a live binding of it would use (its FaceTrackingComponent's settings when it has one),
    // against the take's own clock at opts.fps, the face at full authority from its first frame -
    // and fills `out` with one bone clip (the bones the mapping turns, as local rotations on the
    // rest pose, from the head chain's Neck down; a chain share above the Neck folded into it) and
    // one morph clip (every morph the mapping drives). Touches no component, pose or morph.
    static bool Bake(World& world, SkeletonRegistry& skeletons, Entity character, const FaceTracking::FaceTake& take,
                     const BakeOptions& opts, Resource::AnimationResource& out, BakeReport& report, std::string& error);
    // Bake from a take file to an .ianim file (FaceTracking.Bake, the Face Tracking
    // window): the take is read through AssetFS, the clips are named after its file
    // unless opts.clipName is set, and the .ianim is written atomically (creating its
    // folder when missing).
    static bool BakeTakeFile(World& world, SkeletonRegistry& skeletons, Entity character, const std::string& takePath,
                             const std::string& outPath, const BakeOptions& opts, BakeReport& report, std::string& error);

private:
    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;
    bool              m_enabled = true;
    std::vector<FaceTracking::FaceFrame> m_frames; // ReadNew's scratch, kept warm
};
