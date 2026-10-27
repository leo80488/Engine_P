#pragma once

// ChainPhysicsSystem - Verlet integration + PBD distance-constraint solver for hair / skirt /
// cloth bone chains, plus spring-damper SpringBones for single jiggle bones (chest, butt).
// Per frame (CPU, before LocalToWorldSystem): AnimationSystem + IK + PostIKGrants fill the poses ->
// ChainPhysicsSystem::Update simulates and writes them -> LocalToWorldSystem accumulates world matrices.
// What simulates: the authored ChainPhysicsComponent::groups; with none, bones are classified by
// name (ChainPhysicsNames::ClassifyBone): e.g. Hair / PonyTail -> hair strands, Skirt / Coat ->
// ring-tied cloth strands, Chest / Butt -> spring bones.

#include "ECS/ECS.h"
#include "ECS/AnimationSystem.h"
#include "Resource/SkeletonAsset.h"

#include <DirectXMath.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <cstdint>
#include <utility>

// ==== Authored chain groups (KawaiiPhysics style) ====
// Pick a root bone -> physics covers its WHOLE descendant subtree, minus the excluded bones
// (optionally with their subtrees). Non-empty ChainPhysicsComponent::groups decide which bones
// simulate; with none, the bone-name keyword path (ClassifyBone) runs instead.
// Bones are stored by NAME (resolved to an index in InitEntity), so a re-import that shifts bone
// indices does not corrupt authored chains.
// Physics type, independent of the use-case name: Chain = an independent Verlet strand (hair / rope
// / antenna / tail); Cloth = a ring of chains tied by horizontal + shear constraints (skirt / cape /
// coat / dress); Spring = one spring-damper jiggle bone (chest / belly). The values are serialized:
// keep the order.
enum class ChainGroupType : uint8_t
{
    Chain  = 0,  // independent verlet chain  (solver StrandType::Hair)
    Cloth  = 1,  // ring-constrained cloth     (solver StrandType::Skirt)
    Spring = 2,  // single spring-damper jiggle bone
};

struct ChainGroupDef
{
    static constexpr int MAX_EXCLUDE = 16;

    char           name[48]     = {};   // editor label, free text
    char           rootBone[64] = {};   // SERIALIZED IDENTITY = bone NAME (matches boneNames[i][64])
    ChainGroupType type         = ChainGroupType::Chain;
    bool           enabled      = true;

    // ---- Exclusion (KawaiiPhysics ExcludeBones) ----
    char  excludeBones[MAX_EXCLUDE][64] = {}; // bone NAMES dropped from this group's descent
    int   excludeCount   = 0;
    bool  excludeSubtree = true;  // true: drop the bone AND its descendants; false: only the bone

    // ---- Skirt ring identity (reproduces ParseSkirtID grouping) ----
    int   ringGroup = 0;    // shared horizontal/shear constraint set id
    int   ringIndex = -1;   // position around the ring; -1 = append order

    // ---- Spring pairing ----
    int   pairGroupId = -1; // >=0: Spring groups sharing this id get a virtual midpoint root

    // ---- Per-group physics override (KawaiiPhysics per-chain material) ----
    // ovrEnabled false: the group uses the type-global params on ChainPhysicsComponent. true: these
    // values replace them for THIS group only (a stiff scarf and floppy hair can both be Chain). For
    // Cloth the editor keeps these fields in sync across every chain sharing ringGroup (one piece-level
    // material). The automatic root->tip stiffness falloff still applies on top.
    bool  ovrEnabled = false;
    // Chain / Cloth (Verlet+PBD) overrides:
    float ovrDamping        = 0.15f; // velocity retention (lower = settles faster)
    float ovrGravity        = -10.f;
    // PBD stiffness per CONSTRAINT BUCKET, not per group type: the vertical along-chain links, the
    // horizontal ring links and the diagonal shear links are three independent materials.
    float ovrStructuralStiffness = 1.5f; // vertical / along-chain links
    float ovrRingStiffness       = 1.5f; // Cloth only: horizontal hoop links
    float ovrShearStiffness      = 1.5f; // Cloth only: diagonal cross-level links
    float ovrMaxVelocity    = 0.5f;
    float ovrLocalStiffness = 0.05f; // pull toward rest pose (higher = holds shape)
    float ovrHorizDecay     = 1.0f;  // Cloth only: ring+shear stiffness root->tip
    // Floor for the root->tip "hold shape" falloff. The pull blend is
    // localStiffness * max((1-t)^2, ovrTipHold), so 0 = tip fully free (classic
    // floppy chain), 1 = whole chain held uniformly. Raise it for short bang
    // chains that must keep their silhouette (avoids the gravity=-1000 hack).
    float ovrTipHold        = 0.f;
    // Cloth collision thickness for this group: replaces the component's collisionPadding when
    // ovrEnabled. Added to the capsule radius for this group's particles, so a thick coat and a thin
    // lining can share one body collider.
    float ovrCollisionPadding = 0.02f;
    // Per-group contact friction (see ChainPhysicsComponent::contactFriction).
    // A heavy coat that should drape and grip and a satin lining that should
    // slide are the same capsules with different friction.
    float ovrContactFriction  = 0.20f;

    // ---- Rest-pose limit (KawaiiPhysics LimitAngle) ----
    // Max angle (deg) a bone's simulated direction may deviate from its ANIMATED rest direction;
    // 0 = unlimited. Unlike localStiffness (a spring: a hard impulse still drags the chain arbitrarily
    // far before it decays) this is a hard bound, which is what lets a group opt out of collision.
    float ovrLimitAngle    = 0.f;
    // Cone widening root->tip. Defaults to 2, not 1 (uniform): the local pull's falloff is (1-t)^2,
    // so the root is already held and never reaches its cone, and a uniform cone is hit only by the
    // TIP ("the tip went rigid and the root did nothing"). Widening keeps the limit a safety bound
    // instead of a second shape control competing with Hold shape.
    float ovrLimitTipScale = 2.f;

    // ---- Motion response ("Inertia") ----
    // Fraction of the root bone's per-frame motion the chain FEELS: 1 = full physical reaction; 0 = the
    // chain translates rigidly with its root and only gravity / sway move it. Scales the reaction down
    // WITHOUT stiffening the chain, so small-amplitude motion stays lively.
    float ovrInertiaScale  = 1.f;

    // ---- World-motion inertia (see ChainPhysicsComponent::worldInertiaLocation) ----
    // This group's OWN share of the entity's world motion (a stiff ornament and a long cape on one
    // character want different values); < 0 = follow the component's value (the default). NOT gated by
    // ovrEnabled, like `collide`: the authored rigs override every group, so an ovr-gated value would
    // leave the entity-level slider dead on exactly those rigs. Read by Chain, Cloth and Spring groups.
    float ovrWorldInertiaLocation = -1.f;
    float ovrWorldInertiaRotation = -1.f;

    // ---- Ambient idle sway ----
    // Amplitude is a direct rest-target OFFSET in world units at the strand tip,
    // so it does not interact with stiffness. Rides the local-pull, hence it
    // needs localStiffness > 0 for this group.
    float ovrWindStrength  = 0.f;
    float ovrWindFreq      = 0.6f;  // Hz

    // ---- Spring (jiggle) overrides ----
    // The virtual-root pass (ovrSpring*) and the child pass (ovrSpringChild*) are two materials, as in
    // the globals (root 120/12/1.0/-2.0, children 350/20/0.5/-0.5). The ovrSpringChild* defaults
    // deliberately MIRROR the root defaults, not the springChild* globals, and the loader defaults each
    // missing child key to the group's own root value, so an override saved without the child keys still
    // drives the children with the root's spring set.
    float ovrSpringStiffness = 120.f;
    float ovrSpringDamping   = 12.f;
    float ovrSpringMass      = 1.f;
    float ovrSpringGravity   = -2.f;
    float ovrSpringMaxDisp   = 2.f;
    float ovrSpringChildStiffness = 120.f;
    float ovrSpringChildDamping   = 12.f;
    float ovrSpringChildMass      = 1.f;
    float ovrSpringChildGravity   = -2.f;
    float ovrSpringChildMaxDisp   = 2.f;

    // ---- Collision opt-out (perf) ----
    // Deliberately NOT gated by ovrEnabled: this is a cost decision, not a
    // physics material. SolveCapsuleCollision runs nSub*(1+iterations)+1 times
    // per island per frame and is the system's dominant cost, so a chain whose
    // limit cone provably cannot reach the body (bangs, ribbons, hair ornaments)
    // is worth excluding. An island skips collision only when EVERY group in it
    // opts out.
    bool  collide = true;
};

// BoneRestPose - "the tail was modelled straight but has to hang curled": bone `boneName` holds
// local rotation `rot`.
// Stamped into AnimationSystem's LocalPose in Update's serial phase BEFORE InitEntity, the no-clip
// resetRot cleanup and the FK anchor cache, which makes it the solver's REST shape rather than a
// one-shot pose: every rest reference (the local-pull target, the LimitAngle cone axis, the
// kinematic root snap, the inertia carry, the spring goals) is FK over this LocalPose, so the chain
// hangs at the authored shape, swings from body motion and relaxes back to it.
// ROTATION ONLY: a rotation keeps each bone's local translation, which keeps Constraint::restLength,
// Strand::maxReachSq, SpringBone::restLocalPos and WriteBonePoses' aim reference
// (poses[childBone].pos) valid. Do not add pos / scl fields.
// ABSOLUTE, not a delta from bind: re-applying it every frame cannot accumulate. A source-less
// skeleton's LocalPose is seeded only once, so composing onto poses[b].rot would compose onto its
// own previous output.
// Bone identity is by NAME (like ChainGroupDef::rootBone), so a re-import that shifts bone indices
// cannot move the pose onto another bone.
struct BoneRestPose
{
    char              boneName[64] = {};             // serialized identity
    DirectX::XMFLOAT4 rot = { 0.f, 0.f, 0.f, 1.f };  // ABSOLUTE local rotation, parent-relative, xyzw
    float             weight  = 1.f;                 // per-bone blend [0..1], times the component weight
    bool              enabled = true;

    // ---- runtime cache (not authored data, not serialized) ----
    uint32_t cachedBoneIdx    = ~0u;
    char     resolvedName[64] = {};
    uint32_t resolvedSkel     = 0xFFFFFFFFu;  // SkeletonComponent::assetIndex the cache was built for
    // A resolve FAILURE has to latch too. ResolveBoneByName returns ~0u when the
    // name is absent, so storing that straight into cachedBoneIdx would make the
    // staleness test true again next frame and re-scan every name against every
    // bone, forever, behind a single log line.
    bool     resolveFailed    = false;
    bool     warnedMissing    = false;        // LOG_WARNING once per name

    // Did ApplyRestPose write this bone THIS frame? IsRestPosedBone reads only this flag, which
    // guarantees that the bones resetRot skips are exactly the bones ApplyRestPose wrote. Do not
    // re-derive the condition on the read side: it eventually disagrees, and a bone that is skipped but
    // never re-based drifts, chasing itself.
    bool     appliedThisFrame = false;
};

// ECS component: marks an entity for chain physics simulation.
struct ChainPhysicsComponent
{
    // Soft cap on `groups` (the hard ceiling: see groups).
    static constexpr int MAX_GROUPS = 250;
    static_assert(MAX_GROUPS < 0xFF,
                  "group indices ride in a uint8_t with 0xFF reserved for 'no group'");

    // Cap on `restPose` entries.
    static constexpr int MAX_REST_POSE_BONES = 128;  // soft cap: clamp+warn on load, enforced by the UI

    bool  enabled         = true;

    // ---- Chain (hair-strand) parameters ----
    float damping         = 0.8f;    // velocity retention per frame (lower = settles faster)
    float gravity         = -30.0f;  // Y-axis gravity strength
    // VERTICAL (along-chain) PBD stiffness for Chain AND Cloth: the vertical constraint builder has no
    // type test, so a Cloth piece's structural links read THIS value too. Not a per-type preset (see
    // ringStiffness).
    float structuralStiffness = 1.5f;
    int   iterations      = 7;       // constraint iterations per frame
    float maxVelocity     = 0.5f;    // velocity clamp (world units/frame, 0=unlimited)
    float localStiffness  = 0.02f;   // pull toward rest pose (0=free, 1=locked)
    // Floor for the root->tip hold-shape falloff (the math is at ChainGroupDef::ovrTipHold); 0 = tip
    // fully free. A global because ovrTipHold needs ovrEnabled, and the keyword path (no groups) has no
    // group to override from.
    float tipHold         = 0.f;
    int   substeps        = 1;       // minimum collision substeps (1..8; fast capsules auto-raise it)
    float limitAngle      = 0.f;     // max swing from the animated rest dir (deg); 0 = unlimited
    float limitTipScale   = 2.f;     // cone widening root->tip (see ovrLimitTipScale)
    float inertiaScale    = 1.f;     // 1 = full reaction to body motion, 0 = rides the root

    // ---- World-motion inertia (KawaiiPhysics World Damping / Teleport) ----
    // The solver simulates in the SKELETON's own space (FK over the LocalPose, no entity transform), so
    // moving the ENTITY (a KCC, a turn in place, root motion, a teleport) is invisible to it. These add
    // the entity's world motion back as INERTIA: every simulated particle and spring bone keeps its
    // WORLD velocity when the entity's velocity changes, so chains lag a start, overshoot a stop, swing
    // out of a turn and flare while the entity spins. Velocity-level, not positional: a steady walk or
    // turn is invisible, and the kick goes through the Verlet history, so damping and maxVelocity shape
    // and bound it like any other motion.
    //   Location / Rotation: the fraction of the entity's translation / rotation the chains feel, [0,1].
    //   0 = the chains ride the entity rigidly (byte-identical to a solve without world inertia);
    //   1 = the physical amount. KawaiiPhysics' WorldDampingLocation / Rotation are the complement
    //   (1 - these). Per group: ChainGroupDef::ovrWorldInertia* (< 0 = follow these; not ovrEnabled-gated).
    //   Teleport distance / angle: a solver step whose entity motion exceeds either (world units /
    //   degrees per step - per frame on the main lane) is a teleport: its world motion is ignored (no
    //   whip) and the velocity from before the jump carries on. 0 = no threshold.
    float worldInertiaLocation = 0.f;
    float worldInertiaRotation = 0.f;
    float teleportDistance     = 0.5f;   // world units per solver step
    float teleportAngle        = 30.f;   // degrees per solver step

    // ---- Cloth collision thickness ----
    // Added to EVERY capsule's radius when colliding this character's cloth, so a capsule authored to
    // the true body surface still keeps the cloth a thickness OFF the body (the particle is the bone
    // centerline, not the skinned face). 0 = the bone rests on the capsule surface.
    // Per group: ChainGroupDef::ovrCollisionPadding.
    float collisionPadding = 0.0f;   // world units (try 0.02..0.05)

    // ---- Contact response ----
    // contactSkin inflates the contact radius MULTIPLICATIVELY so cloth rests a hair off the surface,
    // which stops it toggling in and out of contact every frame. Not a duplicate of collisionPadding:
    // padding is an absolute authored thickness (bone centerline -> skinned surface), skin a
    // proportional numerical margin that scales with the collider. Use padding to move the cloth off the
    // body, skin only if contact still chatters after that. Clamped to >= 1 at use (below 1 the cloth
    // sinks INTO the capsule).
    float contactSkin     = 1.015f;
    // Fraction of tangential velocity REMOVED on contact: 0 = frictionless slide, 1 = full grip. Raise
    // it when cloth shivers ALONG the body rather than popping in and out. Applied once per substep, on
    // the swept pass only: the static per-iteration passes skip it, or its strength would scale with
    // cfg.iterations. Per group: ChainGroupDef::ovrContactFriction.
    float contactFriction = 0.20f;

    // ---- Cloth (StrandType::Skirt) presets ----
    // Per-strand-type presets of the Chain fields above: the solver reads them through the same
    // expressions, picking the source by `isSkirt`. They cannot fold into the Chain fields: the keyword
    // path (no groups) has no ChainGroupDef to override from, so these are that path's only
    // hair-vs-cloth discriminator. A negative value is used as is (no fall-back-to-hair sentinel).
    float skirtDamping        = 0.15f;   // cloth settles faster than hair
    float skirtGravity        = -5.0f;   // less aggressive gravity
    float skirtMaxVelocity    = 0.5f;    // tighter velocity clamp
    float skirtLocalStiffness = 0.3f;   // stronger pull toward rest pose
    float skirtTipHold        = 0.f;    // see tipHold
    float skirtLimitAngle     = 0.f;    // see limitAngle
    float skirtLimitTipScale  = 2.f;
    float skirtInertiaScale   = 1.f;    // see inertiaScale

    // ---- Cloth ring materials (NOT presets: separate constraint buckets) ----
    // A Cloth piece ties adjacent chains with HORIZONTAL links (same depth, neighbouring chains) and
    // diagonal SHEAR links (cross-level). Ring stiffness resists the hoop widening, shear resists
    // scissoring / twisting, independently (from flowy separate panels to a rigid barrel). Cloth
    // strands only; a Chain group owns no constraint in these buckets.
    float ringStiffness       = 2.0f;   // horizontal (hoop) links
    float shearStiffness      = 2.0f;   // diagonal (cross-level) links
    float skirtHorizDecay     = 1.0f;   // ring + shear stiffness decay root->tip (0 = tip has none, 1 = uniform)

    // ---- Ambient idle sway (Chain + Cloth) ----
    // A small continuous drift keeps a heavily rest-limited chain from reading as rigid. It is a
    // time-varying offset of the local-pull REST TARGET (not a force), so windStrength IS the tip
    // amplitude in world units regardless of stiffness, damping or frame rate, and it can never push a
    // bone outside its limit cone (measured against the same rest). windStrength 0 disables it (the
    // default). Per-group scale: ChainGroupDef::ovrWindStrength / ovrWindFreq.
    DirectX::XMFLOAT3 windDir = { 1.f, 0.f, 0.f };  // world direction (normalized at use)
    float windStrength   = 0.f;   // world units of tip offset
    float windFreq       = 0.6f;  // Hz
    float windTurbulence = 1.f;   // per-bone phase spread; 0 = whole strand in sync

    // ---- Spring bone (jiggle) parameters - virtual root ----
    float springStiffness = 120.0f;  // spring constant k (50..300)
    float springDamping   = 12.0f;   // damping coefficient d (1..30)
    float springMass      = 1.0f;    // mass - affects inertia (0.1..10)
    float springGravity   = -2.0f;   // Y-axis gravity for subtle droop
    float springMaxDisp   = 2.0f;    // max displacement from goal (world units)

    // ---- Spring bone child parameters (L/R relative to virtual root) ----
    float springChildStiffness = 350.0f;  // very stiff - follows the root closely
    float springChildDamping   = 20.0f;
    float springChildMass      = 0.5f;    // low mass - fast response
    float springChildGravity   = -0.5f;
    float springChildMaxDisp   = 0.5f;    // small displacement from root

    // ---- Explicit authored chains (KawaiiPhysics style) ----
    // Non-empty: these groups, not the bone-name keyword classification, decide which bones simulate.
    // Empty: the keyword path (ClassifyBone).
    // Heap-backed, NOT an inline array: ChainGroupDef is ~1 KB (exclude-name table) and
    // World::AddComponent takes the component by value, so a fixed array of 64 groups (~76 KB)
    // overflows the stack on scene load. MAX_GROUPS is a soft cap (editor UI and deserializer; a
    // per-column skirt rig reaches 51 groups before any extra chain). The HARD ceiling is the group
    // index format: Strand / Constraint / SpringBone carry it as a uint8_t with 0xFF = none (type
    // globals), and GroupStiffnessTables is 256 wide, so the cap can go to 254 and no further (the
    // static_assert).
    std::vector<ChainGroupDef> groups;

    // ---- Authored rest pose (see BoneRestPose) ----
    // Heap-backed for the same reason as `groups`: World::AddComponent and ComponentPool<T>::Add take
    // the component BY VALUE, and the deserializer adds a third copy.
    std::vector<BoneRestPose> restPose;

    // Master switch. Keeps the authored content so a rig can be A/B'd against the
    // modelled (bind) shape without throwing the work away.
    bool  restPoseEnabled = true;

    // Slerp from skel.restPoseLocal[b] (the modelled shape) toward the authored rotation; 1 = plain
    // absolute write (the idempotent fast path). The base is deliberately NOT poses[b].rot: ApplyRestPose
    // explains why that would creep.
    float restPoseWeight  = 1.f;

    // Copies the AUTHORED fields of this component into @p dst field by field - never `dst = *this`:
    // that would clobber BoneRestPose's six runtime-cache fields (cachedBoneIdx, resolvedName,
    // resolvedSkel, resolveFailed, warnedMissing, appliedThisFrame): every name re-resolved every
    // frame, one LOG_WARNING per missing bone per frame, appliedThisFrame wiped between ApplyRestPose
    // and resetRot. dst keeps its own cache for every rest-pose entry whose bone NAME is unchanged at
    // that index; a renamed / new entry starts fresh. Defined next to the ChainPhysics serializer
    // (src/Resoruce/ComponentSerializers.cpp) so the authored-field list has ONE home: a field added
    // to the writer goes there too.
    void CopyAuthoredInto(ChainPhysicsComponent& dst) const;
    // The same authored-field list, COMPARED (runtime caches ignored, char arrays as C strings). The
    // preview panel syncs its staged copy into the stage, and records a panel-undo step, only when
    // this is false.
    bool AuthoredEquals(const ChainPhysicsComponent& other) const;
    // The PHYSICS MATERIAL half of the two above: every authored field EXCEPT the rest-pose block
    // (restPose / restPoseEnabled / restPoseWeight), which dst keeps. The preview's Apply writes this
    // onto a prefab-REF instance (SceneSerializer saves an instance's rest pose per instance as a CPR
    // line; its material follows the .ipfb). One field list (an X-macro in ComponentSerializers.cpp)
    // feeds all four.
    void CopyMaterialInto(ChainPhysicsComponent& dst) const;
    bool MaterialEquals(const ChainPhysicsComponent& other) const;
};

// CapsuleColliderComponent - per-skeleton capsule colliders for cloth / hair, on the same entity
// as SkeletonComponent + ChainPhysicsComponent. Each capsule is two bone endpoints + a radius.
struct CapsuleColliderComponent
{
    static constexpr int MAX_CAPSULES = 16;

    struct CapsuleDef
    {
        uint32_t boneA    = 0;       // skeleton bone index for endpoint A
        uint32_t boneB    = 0;       // skeleton bone index for endpoint B
        float    radius   = 0.05f;   // collision radius (world units)
        bool     enabled  = true;

        // Fine-tuning offsets applied in the bone's local space.
        DirectX::XMFLOAT3 offsetA = { 0.f, 0.f, 0.f }; // local offset from boneA
        DirectX::XMFLOAT3 offsetB = { 0.f, 0.f, 0.f }; // local offset from boneB
    };

    CapsuleDef capsules[MAX_CAPSULES];
    int        count = 0;
};

class ChainPhysicsSystem
{
public:
    // ---- Distance LOD ----
    struct LodTiers
    {
        // Distances are to the camera, in world units.
        float tier1Distance = 18.f;
        float tier2Distance = 35.f;
        // Hysteresis band. Without it a character parked on a threshold
        // chatters between "resting on the leg" and "sinking into it".
        float hysteresis    = 4.f;
        // SHADOW-ONLY profile: a character admitted by a shadow cascade but not the camera is already
        // demoted one tier; on top of that a tier-1 shadow-only solve is capped at this many iterations and
        // substeps (such a character costs ~0.33 ms-core against 0.09 at tier 2). Collision stays ON:
        // without it hair and skirts sink into the body and snap out in one frame when the character turns
        // into view. shadowOnlyIterations 0 = no cap (the one-tier demotion only). Lua
        // ChainLOD.SetTiers{shadowIterations, shadowSubsteps}; -chainlodshadow=<iterations> overrides it for
        // A/B runs.
        int   shadowOnlyIterations = 1;
        int   shadowOnlySubCap     = 1;
        // OPT-IN, a visible change (an off-screen character's shadow hair / skirt updates at half rate). A
        // shadow-only entity solves every OTHER frame (staggered by entity id, so the population splits
        // evenly) with the dt it held; on the frame in between its bones are re-written from the particles
        // as they are (the preview lane's HoldPass logic, per entity, in parallel). The rate-LOD objection
        // (see lodTiers) does not apply: substep saturation is moot at shadowOnlySubCap (1) substeps, and an
        // entity holds only while 2 x dt fits under Simulate's 1/30 clamp (60 Hz and up).
        // -chainlodshadowrate enables it for A/B runs; it needs lodEnabled like every tier.
        bool  shadowOnlyHalfRate   = false;
    };

    // ---- Preview lane ----
    // The editor's Chain Physics Preview panel simulates its hidden stage while the editor is STOPPED,
    // where the scene's own chain physics is off (App gates m_simulationEnabled on runUpdate, and animDt
    // is 0). Update therefore runs up to TWO passes of the solver per frame, SERIALLY:
    //   Main    - the scene, minus the preview set;
    //   Preview - the preview set alone, on the panel's own dt and sway phase (m_previewTime).
    // The sets are disjoint (the main lane EXCLUDES the preview set), so no entity is integrated twice
    // in a frame. Do not run the two passes in parallel: RunPass's island dispatch is a flat ParallelFor
    // and TaskSystem does not work-steal, so a concurrent second pass is a nested ParallelFor (it hangs
    // on low core counts).
    enum class Lane : uint8_t { Main = 0, Preview = 1 };

    static constexpr std::uint32_t kMaxPreviewStepsPerFrame = 8;

    static constexpr std::uint32_t kMaxPreviewSceneCapsules = 2;

    struct PreviewLaneStats
    {
        std::uint32_t entities              = 0;  // jobs the last preview pass solved
        std::uint32_t substeps              = 0;  // their summed nSub
        std::uint32_t excludedByPreviewLane = 0;  // main-lane skips, last frame
        std::uint64_t solvedBothLanes       = 0;  // entities integrated by BOTH lanes in one frame (invariant: 0)
        std::uint64_t restPoseRecordChurn   = 0;  // preview-entity rest-pose records re-created / swept (invariant: 0)
        std::uint64_t passes                = 0;  // preview RunPass calls
        std::uint64_t pausedFrames          = 0;  // frames the lane was installed but paused (dt <= 0)
        std::uint64_t frames                = 0;  // frames the lane was installed
        float         serialMs = 0.f, parallelMs = 0.f, cpuMs = 0.f;   // last stepped frame
        // The StageClock-driven steps and holds:
        std::uint32_t steps                 = 0;  // passes THIS frame (0 on hold / paused frames)
        std::uint32_t maxStepsPerFrame      = 0;  // session high-water of `steps`
        std::uint64_t interpolatedPasses    = 0;  // passes run on an interpolated target (k < n)
        std::uint64_t holdFrames            = 0;  // frames with dt > 0 and 0 steps (HoldPass ran)
        std::uint32_t holdEntities          = 0;  // last hold: entities whose pose it re-wrote
        std::uint32_t heldBones             = 0;  // last hold: bones in the solver's write range
        // last hold: of those, bones whose held rotation DIFFERS from the raw
        // (unsimulated) input it replaced - "the hold shows the swing, not the
        // clip". 0 on a hold that wrote nothing.
        std::uint32_t holdDiffBones         = 0;
        // The distance-LOD outcome the preview lane's jobs ran with, over the session. The panel forces
        // lodEnabled OFF while it is open, or the stage (~4 km from the main camera) would run tier 2 =
        // 1 iteration, 1 substep, no collision. maxTier = the highest job.tier seen, minIterations the
        // lowest job.iterations (~0u = no pass yet).
        std::uint32_t maxTier               = 0;
        std::uint32_t minIterations         = ~0u;
        // The preview scene the LAST preview pass ran with: scene capsules appended to its jobs (summed
        // over jobs) and whether the scene gravity override was carried (1) or the authored path (0).
        std::uint32_t sceneCapsules         = 0;
        std::uint32_t gravityOverride       = 0;
        // World-motion inertia (ChainPhysicsComponent::worldInertia*), the
        // session: entity-passes that applied a kick; entity-passes whose step
        // was a TELEPORT (world motion ignored); the largest entity speed
        // (world units / s), turn rate (deg / s) and frame acceleration at the
        // entity origin (world units / s^2) a kicking pass saw.
        std::uint64_t worldKicks            = 0;
        std::uint64_t worldTeleports        = 0;
        float         worldMaxSpeed = 0.f, worldMaxTurnDeg = 0.f, worldMaxAccel = 0.f;
    };

    // ---- Solver-state snapshot ----
    // A READ-ONLY copy (FillDebugSnapshot) of one initialized entity's solver state for the preview
    // panel's overlay / stats / -chainpreview_dump, as the LAST pass left it; the solve never reads or
    // writes it.
    // SPACE: every position is in the entity's SKELETON space (the solver's: FK from the LocalPose, no
    // entity transform). A caller drawing in the world multiplies by the same skeleton -> world
    // transform it uses for the bones.
    struct DebugSnapshot
    {
        struct Capsule { DirectX::XMFLOAT3 a, b; float radius; std::uint8_t scene; std::uint8_t interpenetrating; };

        std::vector<DirectX::XMFLOAT3> particlePos;
        std::vector<float>             particleRadius;   // Particle::radius (cloth thickness)
        std::vector<std::uint32_t>     particleBone;     // the skeleton bone it tracks
        // The bone this particle's strand WRITES (WriteBonePoses' range:
        // writesBone, not the tip, not a non-rootSolo root), ~0u otherwise -
        // a bone above a fork is copied into every leaf strand and exactly one
        // copy owns the write.
        std::vector<std::uint32_t>     particleWritesBone;
        std::vector<std::uint8_t>      particleIsland;   // index into the entity's islands (0xFF past 254)
        std::vector<std::uint8_t>      particleGroup;    // the strand's ChainGroupDef index (0xFF none)
        std::vector<std::uint8_t>      particlePinned;   // invMass == 0 (a kinematic root)
        // 0 inside its rest-limit cone (or unlimited), 1 ON the cone (within
        // kLimitTouchDeg of it - the solver is holding it there, PhAT's
        // bDrawViolatedLimits), 2 BEYOND it by more than that (collision owns
        // the segment - ApplyRestLimit's infeasible-cone skip). Needs `cfg`.
        std::vector<std::uint8_t>      particleLimitViolated;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> links;   // particle index pairs
        std::vector<std::uint8_t>      linkBucket;       // 0..5 = constraintGroups[g]: 0-1 vertical, 2-3 ring, 4-5 shear

        std::vector<Capsule>           capsules;         // the last pass's colliders (skeleton space)
        std::uint32_t strands = 0, islands = 0, springBones = 0, groupsSimulated = 0;
        std::uint32_t ownCapsules = 0;                   // bone-attached (not the preview scene's)
        int           lodTier = 0;                       // the last pass's LOD tier (2 = no collision)
        // Groups whose authored collide was switched OFF by the LOD tier this
        // pass (tier 2) - "LOD skipped collision". Needs `cfg` (authored flags).
        std::uint32_t skippedCollideGroups = 0;
        // A TRUE capsule-capsule test (the closest distance between the two axis SEGMENTS against
        // ra + rb) over the entity's OWN capsules (the preview scene's floor proxy excluded): capsules
        // involved / distinct pairs.
        std::uint32_t interpenetratingCapsules = 0, interpenetratingPairs = 0;
        std::uint32_t limitOnCone = 0, limitBeyond = 0;
        // The per-group solo (SetSoloGroup) and the pending pokes (ApplyExternalImpulse).
        std::vector<std::uint8_t>      particleFrozen;   // 1 = its strand is FROZEN AT REST
        int           soloGroup = -1;                    // the group InitEntity kept simulating (-1 none)
        std::uint32_t frozenStrands = 0, frozenGroups = 0, frozenParticles = 0, frozenSprings = 0;
        // max |position - animated rest| over the frozen particles (0 = exactly
        // at rest, the contract) and over the simulated free ones.
        float         frozenMaxDeviation = 0.f, activeMaxDeviation = 0.f;
        std::uint32_t pendingImpulses = 0;
    };
    static constexpr float kLimitTouchDeg = 0.5f;

    // ---- Preview lane: solo, poke, the A/B pose feed ----
    static constexpr float         kImpulseMinBlend      = 1.f / 240.f;
    static constexpr float         kImpulseMaxTtl        = 1.f;
    static constexpr std::uint32_t kMaxPendingImpulses   = 16;
    static constexpr std::uint64_t kImpulseMaxAgeUpdates = 600;

    // ---- Distance LOD ----
    void SetCameraPosition(const DirectX::XMFLOAT3& p) { m_cameraPos = p; }

    // Entities off screen and kept alive only by a shadow cascade. They are demoted at least one tier
    // regardless of distance (a silhouette at shadow resolution does not need full cloth convergence),
    // but never skipped: a caster that stops animating pops in the shadow map.
    void SetShadowOnlySet(const std::unordered_set<Entity>* s) { m_shadowOnly = s; }

    // ---- Preview lane ----
    // The caller OWNS @p ents and keeps it alive until it pushes nullptr. The set is read only in
    // Update's SERIAL phase (and by the ApplyRestPoseOnly sweep) and must be rewritten only outside
    // Update (the panel does it in the editor's ProcessPendingActions window). Pushing a set after a
    // null one starts a new preview session (sway phase and lane counters restart).
    // @p dt <= 0 or non-finite = PAUSED: the lane's entities get the Edit-mode treatment
    // (ApplyRestPoseOnly), like the main lane with the transport stopped.
    // @p steps = fixed steps of dt to run THIS frame (the panel's StageClock yields 0..4 per editor
    // frame and advances its clip clock by the same count, so chain time and clip time are one clock at
    // any editor rate); clamped to kMaxPreviewStepsPerFrame.
    //   steps >= 1: that many serial RunPass calls. The animated pose is sampled once per editor frame,
    //   at its END time; pass k of n gets it interpolated k/n of the way from the PREVIOUS preview
    //   frame's input (lerp pos / scl, slerp rot; a bone equal in both is copied, so a still pose stays
    //   bit-exact) and pass n the frame's own input, so a 30 Hz editor running two 1/60 steps drives the
    //   chains through the targets a 60 Hz editor samples. With no previous input (the first frame,
    //   after a reset or a paused frame) every pass gets this frame's pose. The input is restored before
    //   every pass, or pass 2 would read pass 1's simulated rotations as its target. n == 1 is one plain
    //   pass.
    //   steps == 0: HOLD - no integration, no InitEntity; HoldPass re-writes the last simulated
    //   rotations from the particle state, so a frame with no step (an editor faster than the fixed
    //   step, the panel's Pause) never flickers to the raw clip pose.
    void SetPreviewLane(const std::unordered_set<Entity>* ents, float dt,
                        std::uint32_t steps = 1);
    const std::unordered_set<Entity>* GetPreviewLaneSet() const { return m_previewSet; }
    float GetPreviewLaneDt() const { return m_previewDt; }
    std::uint32_t GetPreviewLaneSteps() const { return m_previewSteps; }
    // The preview lane's sway phase back to 0: the panel's Reset, and the harness's A/B windows, which
    // must start from bit-identical state. Never touches the main lane's m_time.
    void ResetPreviewSwayTime() { m_previewTime = 0.f; }

    // ---- Preview scene ----
    // Two preview-lane-ONLY settings the panel's Scene section pushes every frame (handed back with
    // SetPreviewLane(nullptr) / ClearAll). Neither is authored: Apply never writes them and the MAIN lane
    // never reads them (only the preview lane's jobs carry them).
    // Scene gravity: the authored gravity is a signed Y MAGNITUDE per strand kind (gravity /
    // skirtGravity / a group's ovrGravity / the spring gravities). With the override active each becomes
    // the VECTOR `down * (-authored)`: (0,-1,0) reproduces the authored value, (1,0,0) turns it
    // sideways, (0,0,0) is the No Gravity diagnostic, and |down| scales it. Inactive = the authored
    // scalar path, untouched. @p down is a WORLD direction: phase 1 maps it into each entity's skeleton
    // space (its GlobalTransform's inverse), keeping |down|.
    void SetPreviewGravity(bool active, const DirectX::XMFLOAT3& down)
    {
        m_previewGravityActive = active;
        m_previewGravityDown   = down;
    }
    bool IsPreviewGravityActive() const { return m_previewGravityActive; }

    // Scene capsules: WORLD-space capsules APPENDED to every preview-lane job's
    // bone-attached ones (the floor's collision proxy - the solver collides
    // with capsules only), mapped into each job's skeleton space through its
    // GlobalTransform's inverse (radius / the entity's scale). Truncated at the
    // job's MAX_CAPSULES; the count the last preview pass actually appended is
    // PreviewLaneStats::sceneCapsules. The arrays are copied; @p n is clamped to
    // kMaxPreviewSceneCapsules, and a null array stores none.
    void SetPreviewSceneCapsules(const DirectX::XMFLOAT3* a, const DirectX::XMFLOAT3* b,
                                 const float* radius, std::uint32_t n);
    std::uint32_t GetPreviewSceneCapsuleCount() const { return m_previewSceneCapCount; }

    // "Is THIS entity's chain being simulated right now?" - the main transport for a scene entity, the
    // preview lane for a preview-set entity. The editor's rest-pose capture must refuse while it is
    // (WriteBonePoses REPLACES mid-chain rotations, so a capture would bake the swing). A HOLDING lane
    // (dt > 0, 0 steps this frame) counts as simulating: the screen still shows the simulated swing.
    bool IsSimulating(Entity e) const
    {
        if (m_previewSet && m_previewSet->count(e))
            return m_previewDt > 0.f && std::isfinite(m_previewDt);
        return m_simulationEnabled;
    }

    // Single-entity reset to rest: drop the entity's simulation state (Invalidate) AND its LocalPose
    // scratch, so the next frame re-samples the clip / re-seeds rest before the solver re-inits. Do not
    // use Renderer::ResetChainPhysicsSimulation for this: it clears the Verlet history of every
    // character in the scene. Serial callers only.
    void ResetPreviewEntity(Entity e);

    // What the preview lane did. entities / substeps describe the LAST preview pass; the three ms
    // fields the last frame that STEPPED (summed over its up to kMaxPreviewStepsPerFrame passes);
    // excludedByPreviewLane the main lane's exclusions in the last frame; the rest accumulate over the
    // preview session.
    const PreviewLaneStats& GetPreviewLaneStats() const { return m_laneStats; }

    // World-motion inertia, BOTH lanes, since construction: kicks applied /
    // teleports skipped (entity-passes). Diagnostics only.
    std::uint64_t GetWorldKickCount() const     { return m_worldKicksTotal; }
    std::uint64_t GetWorldTeleportCount() const { return m_worldTeleportsTotal; }
    // Reverse arm for the world-inertia packs (-chainpreview_world_off): every pass solves as if every
    // world-inertia field were 0, bookkeeping included, so the solve is byte-identical to one with no
    // world inertia authored. Serial callers.
    void SetWorldInertiaSuppressed(bool v) { m_worldInertiaSuppressed = v; }
    bool IsWorldInertiaSuppressed() const  { return m_worldInertiaSuppressed; }
    // Does @p cfg author ANY world inertia (the component's, or a group's own)? False = RunPass reads
    // and writes nothing for it.
    static bool WantsWorldInertia(const ChainPhysicsComponent& cfg);
    // A group's effective share: its own value when >= 0, else the component's.
    // g may be null (a strand / spring with no authored group).
    static float EffectiveWorldInertia(const ChainGroupDef* g, const ChainPhysicsComponent& cfg,
                                       bool rotation)
    {
        const float own = g ? (rotation ? g->ovrWorldInertiaRotation : g->ovrWorldInertiaLocation)
                            : -1.f;
        return own >= 0.f ? own : (rotation ? cfg.worldInertiaRotation : cfg.worldInertiaLocation);
    }
    float GetPreviewSwayTime() const { return m_previewTime; }
    // Preview-set entities currently holding an Edit-path rest-pose record: the witness that
    // restPoseRecordChurn is not vacuous (a churn of 0 over 0 records proves nothing).
    std::uint32_t CountPreviewRestPoseRecords() const;
    // "[CHAINPHYS] preview lane: ..." - the panel emits the closing one (when=<reason>) before it
    // hands the lane back, so the LAST such line in a log describes the whole session. The PERIODIC
    // line (when=periodic) is harness-only: Update prints it every `n` installed frames
    // (SetPreviewLaneLogEvery) only in a WITH_HARNESS build and only after the panel asked for it
    // (under a -chainpreview_* run); 0 = never, the default.
    void LogPreviewLaneStats(const char* when) const;
    void SetPreviewLaneLogEvery(std::uint32_t n) { m_previewLogEvery = n; }

    // Read-only copy of an initialized entity's particles (the panel's
    // -chainpreview_dump). positions = simulated, restPositions = this frame's
    // animated rest. False when the entity has no simulation state yet.
    bool CopyParticleState(Entity e, std::vector<DirectX::XMFLOAT3>* positions,
                           std::vector<DirectX::XMFLOAT3>* restPositions) const;

    // ---- Solver-state snapshot ----
    // Fill @p out (see DebugSnapshot); false when @p e has no initialized simulation state. SERIAL-ONLY
    // (the panel's ProcessPendingActions window, never during Update's ParallelFor).
    // `cfg` (nullable): the entity's authored component - the rest-limit
    // cones and the groups' collide flags live there, not in m_entityData.
    // Without it particleLimitViolated is all 0 and skippedCollideGroups 0.
    bool FillDebugSnapshot(Entity e, DebugSnapshot& out,
                           const ChainPhysicsComponent* cfg = nullptr) const;
    // Closest distance between segments [a0,a1] and [b0,b1] (Ericson,
    // Real-Time Collision Detection 5.1.9). Degenerate segments are points.
    static float SegmentSegmentDistance(const DirectX::XMFLOAT3& a0, const DirectX::XMFLOAT3& a1,
                                        const DirectX::XMFLOAT3& b0, const DirectX::XMFLOAT3& b1);

    // ---- Preview lane: solo, poke, the A/B pose feed ----
    // Three PREVIEW-LANE features, stored per entity, honoured only by Lane::Preview passes (the main
    // lane never reads them), and dropped with the entity (OnEntityDestroyed), by ClearAll and by the
    // lane hand-back (SetPreviewLane(nullptr)). Serial callers only: the panel's ProcessPendingActions
    // window, never during Update.
    // SOLO (PhAT "Selected Simulation"): only group @p groupIdx of @p e simulates. Every other group's
    // strands and spring bones are FROZEN AT REST: kinematic at this frame's ANIMATED position (pinned
    // for the PBD / contact solve, moved with the animated pose every substep, snapped onto it before
    // the pose write), so their bones follow the clip exactly - not stopped where the swing left them,
    // which would pop back on un-solo. InitEntity applies the freeze, so a change re-initialises the
    // entity (ResetPreviewEntity). -1 clears. An entry on an entity outside the preview set is kept but
    // not honoured.
    void SetSoloGroup(Entity e, int groupIdx);
    int  GetSoloGroup(Entity e) const;

    // POKE: an impulse on one particle - a velocity change in SKELETON units / s,
    // SKELETON space (the solver's own; the caller maps a world-space drag
    // through the inverse of the skeleton -> world transform), spread over
    // `blendSeconds` of SIMULATED time as a constant acceleration added beside
    // gravity in IntegrateStrand. It is PENDING on the entity's EntityData and
    // consumed by the preview lane's passes (a hold / paused frame consumes
    // nothing), and it EXPIRES: after blendSeconds of simulated time (the TTL,
    // clamped to [kImpulseMinBlend, kImpulseMaxTtl]), after
    // kImpulseMaxAgeUpdates Updates unconsumed, and on any reset of the entity
    // - so a poke can never pollute the stage permanently. At most
    // kMaxPendingImpulses are pending per entity: a new one drops the oldest.
    // False (nothing queued) for an entity outside the preview set or without
    // simulation state, a pinned / frozen / out-of-range particle, or a
    // non-finite input.
    bool ApplyExternalImpulse(Entity e, std::uint32_t particleIdx,
                              const DirectX::XMFLOAT3& impulseSkeletonSpace,
                              float blendSeconds);
    std::uint32_t GetPendingImpulseCount(Entity e) const;

    // A/B POSE FEED (the A/B ghost's cheap path): @p shadow is a bare preview-set entity carrying ONLY
    // a ChainPhysicsComponent (no skeleton, mesh or animation - never animated, skinned, pinned or
    // drawn). A preview pass solves it with @p source's skeleton, this frame's animated INPUT pose
    // (copied into a solver-owned buffer at the start of the preview lane, before any pass writes the
    // source's pose), GlobalTransform (the world -> skeleton map), capsule colliders and live-pose-source
    // verdict (resetRot). With equal parameters the shadow's particles are bit-identical to the
    // source's, for the cost of one chain solve. NullEntity source = remove the feed.
    void SetPreviewPoseFeed(Entity shadow, Entity source);
    std::uint32_t GetPreviewPoseFeedCount() const
    { return static_cast<std::uint32_t>(m_poseFeeds.size()); }

    explicit ChainPhysicsSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    // Simulate all entities with ChainPhysicsComponent: the MAIN lane (the
    // scene, minus the preview set) and then, when one is installed, the
    // PREVIEW lane (see SetPreviewLane). Reads/writes AnimationSystem::LocalPose
    // via GetMutableLocalPose(). The one production call site is
    // Renderer::TickAnimationChain.
    void Update(World& world, float dt,
                const std::unordered_set<Entity>* activeSet = nullptr);

    // ONE pass of the solver over the ChainPhysicsComponent pool, for one lane: entities in @p exclude
    // are skipped in phase 1; the ambient-sway phase is the @p swayTime argument (RunPass reads neither
    // m_time nor m_previewTime); the assign-style AnimProfile ms fields go to the lane's own set
    // (chain*Ms for Main, previewChain*Ms for Preview). Public for tests/unit/chain_preview_lane_test.cpp;
    // production code calls Update. Serial callers only, never two RunPass calls concurrently.
    void RunPass(World& world, float dt,
                 const std::unordered_set<Entity>* activeSet,
                 const std::unordered_set<Entity>* exclude,
                 float swayTime, Lane lane);

    // Transport gate controlled by App's authoritative PlayModeController.
    // Disabled Update calls perform no initialization, integration, collision
    // solving or SIMULATION LocalPose writes in Edit / Pause.
    //
    // The one exception is the authored rest pose (BoneRestPose), which is
    // AUTHORING DATA rather than simulation: it is stamped in Edit / Pause too,
    // because otherwise dragging the viewport pose gizmo would show nothing. It
    // writes one absolute local rotation per authored bone, allocates nothing and
    // touches no EntityData; with an empty restPose it is a bitwise no-op.
    void SetSimulationEnabled(bool enabled) { m_simulationEnabled = enabled; }
    bool IsSimulationEnabled() const { return m_simulationEnabled; }
    // The scene plays but its time stands still (a time scale of 0 - hit-stop, a paused game
    // state - or the path tracer's time freeze): with the simulation off, the main lane re-writes
    // every chain's bones from its particles as they are (no integration, no sway clock) instead
    // of stamping the rest pose, so the character keeps the hair it had and resumes from it.
    void SetHold(bool hold) { m_hold = hold; }

    // Force re-detection of chains for an entity (e.g. after skeleton change).
    void Invalidate(Entity e);

    // Does going from @p before to @p after need Invalidate? True exactly for the authored data
    // InitEntity BAKES: the group list's topology (count, rootBone, type, enabled, excludes,
    // excludeSubtree, ringGroup / ringIndex, pairGroupId) and the authored rest pose (list, enabled,
    // weight - a rotation moves a cloth piece's ring / shear restLength, a particle-to-particle
    // distance). Every other authored field is read per frame (Simulate / prepareEntity / the group
    // stiffness tables) and applies live. Pure, static, touches no solver state.
    static bool NeedsReinit(const ChainPhysicsComponent& before,
                            const ChainPhysicsComponent& after);

    // ---- Authored rest pose ----
    // Stamp cfg.restPose into @p poses. Rotation-only, ABSOLUTE, idempotent; a strict no-op when
    // cfg.restPose is empty.
    // Serial callers only: it writes cfg's runtime cache fields (cachedBoneIdx / resolvedName /
    // resolvedSkel / resolveFailed / warnedMissing / appliedThisFrame), and one ChainPhysicsComponent
    // is shared by all of the entity's island tasks - never call it from Update's phase-2 ParallelFor
    // or any other thread.
    static void ApplyRestPose(ChainPhysicsComponent& cfg,
                              const SkeletonAsset&   skel,
                              uint32_t               skelAssetIndex,
                              AnimationSystem::LocalPose* poses);

    // Was `bone` actually written by ApplyRestPose this frame? Read by Update's
    // no-clip resetRot loop so it leaves authored bones alone. ApplyRestPose must
    // already have run this frame.
    static bool IsRestPosedBone(const ChainPhysicsComponent& cfg, uint32_t bone);

    // Drop simulation state for a destroyed entity. Wired through
    // World::AddEntityDestroyListener via Renderer::OnEntityDestroyed.
    void OnEntityDestroyed(Entity e)
    {
        m_entityData.erase(e);
        m_editRestPoseApplied.erase(e);
        m_previewRecordSeen.erase(e);   // the id recycles
        DropPreviewPrevPose(e);         // same reason
        DropPreviewM7(e);               // solo entry + pose feeds naming e
    }

    // Clear all cached entity data (call before World::Clear).
    void ClearAll()
    {
        m_entityData.clear();
        // The Edit-path restore set describes rotations we left in a LocalPose
        // cache that is itself about to be dropped (ClearLocalPoseCache runs with
        // this on Play/Stop and world load). Keeping it would restore bind onto a
        // freshly re-seeded pose for one frame.
        m_editRestPoseApplied.clear();
        m_previewRecordSeen.clear();
        // Every id is about to be recycled (world load, Play/Stop), so a caller-owned preview set names
        // nobody now: drop the pointer rather than simulate / exclude whoever inherits those ids. The panel
        // re-pushes after its own World::Epoch() handling (a push after null starts a new session).
        m_previewSet = nullptr;
        m_previewDt  = 0.f;
        m_previewSteps = 1;
        m_previewPrevCount = 0;         // no interpolation origin
        m_previewLogEvery = 0;
        // The preview scene belongs to the dropped session.
        m_previewGravityActive = false;
        m_previewSceneCapCount = 0;
        // So do the solo entries and the A/B pose feeds.
        m_soloGroups.clear();
        m_poseFeeds.clear();
    }

    // Editor overlay query: this frame's bone world matrices (skeleton space)
    // for an initialized entity, or nullptr if no simulation data exists yet
    // (e.g. the frame right after Invalidate, before the next Update). World
    // position of bone b = (m._41, m._42, m._43) of the returned matrices[b].
    const std::vector<DirectX::XMFLOAT4X4>* GetBoneWorldMatrices(Entity e) const;

    // Editor "Auto-detect from bone names": runs the keyword classification (ClassifyBone /
    // ParseSkirtID) once and fills cfg.groups with explicit, editable ChainGroupDef entries, so the
    // keyword logic lives in one place.
    static void BuildGroupsFromNames(const SkeletonAsset& skel,
                                     ChainPhysicsComponent& cfg);

private:
    // ---- Internal data structures ----
    struct Particle
    {
        DirectX::XMFLOAT3 position;      // current simulated position (skeleton space)
        float              invMass;       // 0 = pinned root
        DirectX::XMFLOAT3 prevPosition;  // previous frame position (Verlet)
        uint32_t           boneIndex;     // skeleton bone index
        DirectX::XMFLOAT3 restPosition;  // animation rest position (updated each frame)
        // Tangential velocity retained on contact, resolved per group. Baked in
        // Simulate next to radius because SolveCapsuleCollision is island-scoped
        // and has no access to the config or to a particle's owning group.
        float              friction = 0.20f;
        // Cloth collision thickness, ADDED to the capsule radius so the skinned surface offset from this
        // bone clears the body, not just the bone centerline. Re-baked every frame in Simulate from
        // ChainPhysicsComponent::collisionPadding / a group's ovrCollisionPadding, so tuning needs no
        // Invalidate.
        float              radius = 0.f;
        // Does THIS particle's strand own the pose write for its bone? TraceChains forks into one strand
        // per leaf path, so a bone above a fork is copied into every leaf strand. InitEntity gives each
        // bone exactly one writer, the longest strand through it (ties: first in trace order); with one
        // write per copy a long strand's top links would follow whichever short side branch was traced
        // last and twitch every frame. Tip particles never write (no child to aim at) and keep the default.
        bool               writesBone = true;
    };

    struct Constraint
    {
        uint32_t particleA;
        uint32_t particleB;
        float    restLength;
        float    stiffness;
        float    segmentT = 0.f; // 0 = root, 1 = tip - drives the horizontal stiffness decay
        uint8_t  groupIdx = 0xFF; // source ChainGroupDef index (0xFF = none -> globals)
    };

    enum class StrandType { Hair, Skirt };

    struct Strand
    {
        StrandType type;
        uint32_t   particleOffset; // into EntityData::particles
        uint32_t   particleCount;
        // No ringIndex here: a strand's place around the ring is its ORDER within its cloth piece, which the
        // horizontal / shear builder walks. ChainGroupDef::ringIndex is the authored sort key that produces
        // that order.
        int        clothPieceId = 0;  // skirt only: authored ringGroup, or the keyword path's skirt group
        uint8_t    groupIdx = 0xFF; // source ChainGroupDef index (0xFF = none -> globals)
        // True when this strand is the ONLY strand rooted at its root bone: WriteBonePoses then rotates the
        // root bone toward particle 1 (KawaiiPhysics ApplySimulateResult parity - it rotates only a parent
        // with <= 1 child). Forked chains / authored Cloth rooted at a shared parent keep false, or every
        // strand would overwrite the shared bone, last writer winning.
        bool       rootSolo = false;
        // Divergence safeguard: squared max distance any particle may sit from the strand root (2x the
        // strand's rest extent). Past it the sim has blown up and Simulate snaps the particle back, so a
        // runaway chain never reaches screen-spanning size and TDRs the GPU. 0 = not computed (no clamp).
        float      maxReachSq = 0.f;
        // FROZEN AT REST by a preview-lane solo (SetSoloGroup; set by ApplySoloFreeze right after
        // InitEntity): every particle is pinned (invMass 0) and kinematic at the animated pose. Always false
        // on the main lane.
        bool       frozen = false;
    };

    // SpringBone - a single jiggle bone (Chest, Butt, ...): spring-damper ODE
    // F = -k*(x - rest) - d*v + gravity.
    struct SpringBone
    {
        uint32_t           boneIndex;       // skeleton bone index (0xFFFFFFFF for virtual)
        DirectX::XMFLOAT3 restLocalPos;    // the bone's local translation at InitEntity
        DirectX::XMFLOAT3 currentWorldPos; // simulated position (skeleton space)
        DirectX::XMFLOAT3 velocity;        // velocity, skeleton space (units / s)

        // No per-bone k / d / mass / radius: SimulateSpringBones resolves them from ChainPhysicsComponent
        // (or the group's ovrSpring*) and passes them to simulateOne, and spring bones never collide (a
        // spring island owns an empty particle range, so SolveCapsuleCollision returns at once).

        // Virtual root support (Chest_Root grouping)
        int32_t  parentSpringIdx = -1;  // index into springBones[]; -1 = independent
        bool     isVirtual       = false; // true = no real bone, physics-only grouping node
        uint32_t parentBoneIndex = ~0u;   // for virtual: skeleton parent bone for world xform

        // Virtual root only: this frame's animated midpoint goal (set in SimulateSpringBones
        // Pass 1), read by children in Pass 2 so their goal tracks the body's CURRENT facing.
        DirectX::XMFLOAT3 animMidpoint = { 0.f, 0.f, 0.f };

        uint8_t  groupIdx = 0xFF;   // source ChainGroupDef index (0xFF = none -> globals)
        // Frozen at rest by a preview-lane solo: held AT its goal with zero velocity (no jiggle is written).
        bool     frozen = false;
    };

    // Capsule collider in the solver's skeleton space, computed per frame from CapsuleColliderComponent
    // (m_previewSceneCaps holds world-space ones until phase 1 maps them per job). Declared before
    // EntityData so EntityData can keep last frame's capsules.
    struct WorldCapsule { DirectX::XMFLOAT3 a, b; float radius; };

    // Constraint color groups (for parallel-safe solving).
    // [0..1] = vertical even/odd, [2..3] = horizontal even/odd, [4..5] = shear even/odd
    static constexpr int kMaxGroups = 6;

    // Independent simulation island: a contiguous run of strands (one cloth piece, or one hair root's
    // forked family) whose particles and constraints touch no other island - the unit of WITHIN-entity
    // parallelism. Every per-particle buffer is accessed only in [particleBegin, particleEnd), so all
    // islands of one character can run as concurrent TaskSystem jobs. Built once per entity by
    // BuildIslands (called from InitEntity).
    struct Island
    {
        uint32_t strandBegin   = 0, strandEnd   = 0; // into strands[]
        uint32_t particleBegin = 0, particleEnd = 0; // into particles[]
        uint32_t constraintBegin[kMaxGroups] = {};   // into constraintGroups[g]
        uint32_t constraintEnd[kMaxGroups]   = {};
        bool     springs = false; // this island simulates springBones (at most one)
    };

    // Per-entity physics data (initialized once, simulated each frame).
    struct EntityData
    {
        struct GroupStiffnessTables
        {
            float vert[256];
            float ring[256];
            float shear[256];
            float horizDecay[256];
        };

        // One pending poke (fields: see impulses).
        struct PendingImpulse
        {
            std::uint32_t     particle = 0;
            DirectX::XMFLOAT3 accel{ 0.f, 0.f, 0.f };
            float             remaining = 0.f;
            std::uint64_t     born = 0;
        };

        // Per-group stiffness lookup, indexed by the uint8 group index: a pure function of the
        // frame-constant ChainPhysicsComponent, resolved once per entity per frame in the serial phase -
        // not in Simulate, which runs once per island task (11 per Rossi). Rebuilt unconditionally each
        // frame rather than invalidated on edit: it is cheap, and a version stamp would be one more thing
        // to get wrong when the component is edited from the Inspector, Lua or an undo.
        GroupStiffnessTables groupTables{};

        // Last frame's LOD tier, so the hysteresis band has something to widen
        // against. Runtime only; -1 means "not yet classified".
        int lodTier = -1;
        // LodTiers::shadowOnlyHalfRate: the elapsed time a held frame did not integrate; the next solving
        // pass adds it to its dt.
        float rateCarry = 0.f;

        std::vector<Particle>  particles;
        std::vector<Strand>    strands;

        std::vector<Constraint> constraintGroups[kMaxGroups];
        int groupCount = 0;

        // Within-entity parallelism (see Island). islandTasks groups island indices whose FK ancestor
        // reads overlap another island's written bones (they run serially, in ascending order, on one
        // worker) - normally one island per task.
        std::vector<Island>            islands;
        std::vector<std::vector<int>>  islandTasks;

        // Spring bones (jiggle physics)
        std::vector<SpringBone> springBones;

        // Pre-computed bone world matrices (skeleton space), rebuilt each frame.
        // Avoids repeated parent-chain traversals in UpdateRoots / LocalSpace / capsules.
        std::vector<DirectX::XMFLOAT4X4> boneWorldMatCache;

        // Reused solver scratch. KawaiiPhysics-style collision proxies accumulate
        // link pushes before feeding them back to shared endpoints, avoiding N-fold
        // correction when one particle belongs to several cloth links.
        std::vector<DirectX::XMFLOAT3> poseStartScratch;
        std::vector<DirectX::XMFLOAT3> collisionPushScratch;
        std::vector<float>             collisionWeightScratch;
        // Per-collision-call snapshot of each particle's prevPosition, taken at SolveCapsuleCollision
        // entry: the tunneling side test compares against the position at the START of the pass, and
        // velResponse rewrites prevPosition on every contact.
        std::vector<DirectX::XMFLOAT3> prevPosSnapshotScratch;
        // Per-particle local-stiffness blend factor for the current frame. It depends only on the
        // particle's index in its strand and frame-constant config, but the substep loop needs it per
        // particle PER SUBSTEP and it costs a powf (2.7k per island per frame at the substep ceiling):
        // resolved once per Simulate.
        std::vector<float>             localBlendScratch;
        // Per-constraint PBD projection strength for the current frame, parallel to constraintGroups[g].
        // Every input is frame-constant (c.stiffness / c.segmentT / c.groupIdx baked at InitEntity; the
        // group stiffness tables and the substep count resolved once per Simulate), but the solve visits
        // every constraint nSub x iterations times and the substep compensation costs a powf: hoisted like
        // localBlendScratch.
        std::vector<float>             constraintStiffScratch[kMaxGroups];
        // Per-particle ambient-sway offset for the current frame (world units), added to the local-pull
        // rest target; frame-constant, so resolved once per Simulate. Written for every particle of the
        // island (zero when the group's sway is off): the pull loop skips strands with localStiffness 0 and
        // must never read stale data.
        std::vector<DirectX::XMFLOAT3> windOffsetScratch;

        // Last frame's world capsules: Simulate LERPs the collider from these toward the current pose
        // across substeps, so a fast body bone cannot sweep past the cloth between frames. Empty /
        // size-mismatched = no interpolation that frame (the static current pose).
        std::vector<WorldCapsule> prevCapsules;
        // How many of prevCapsules' TAIL entries are the preview scene's (the floor proxy); read only by
        // FillDebugSnapshot, never by the solve. Written beside prevCapsules, serially.
        std::uint32_t prevSceneCaps = 0;

        // Verlet stores displacement per simulation step. Dynamic collider CCD can
        // change the substep count; FPS can change too. Rescale the displacement
        // by current/previous step duration before the next integration.
        float velocityStepSeconds = 0.f; // history displacement duration; zero on reset

        bool initialized = false;
        bool divergenceLogged = false;  // one-shot guard for the divergence-clamp warning
        uint32_t diagFrame = 0;         // unused: nothing reads or writes it

        // ---- Preview-lane solo / pokes (zero / empty on the main lane) ----
        // The solo this state was initialised with (ApplySoloFreeze) and what it froze.
        int           soloGroup = -1;
        std::uint32_t frozenStrands = 0, frozenGroups = 0, frozenParticles = 0, frozenSprings = 0;

        // Pending pokes (ApplyExternalImpulse): accel = impulse / blend (skeleton
        // units / s^2), remaining = simulated seconds left (the TTL), born = the
        // Update serial it was queued in (the unconsumed-age backstop).
        std::vector<PendingImpulse>    impulses;
        // This pass's per-particle external acceleration, resolved from
        // `impulses` in the SERIAL phase 1 (PrepareExternalImpulses) and read by
        // IntegrateStrand on the island workers. extAccelActive false = the
        // authored integration, byte for byte.
        std::vector<DirectX::XMFLOAT3> extAccel;
        bool                           extAccelActive = false;

        // ---- World-motion inertia (PrepareWorldKick; serial phase 1 only) ----
        // The entity transform this entity's previous pass OF ITS LANE used
        // (decomposed), and that pass's frame delta - the true, unscaled one,
        // in skeleton space: the previous frame VELOCITY the next pass
        // differences against. wmPass = m_lanePasses[wmLane] at the record,
        // so "recorded by the immediately preceding pass" is one compare (an
        // anim-culled / lane-switched entity re-records instead of turning
        // its gap into a kick). Untouched while no world inertia is authored.
        bool              wmPrevValid  = false;
        bool              wmDeltaValid = false;
        std::uint8_t      wmLane       = 0;
        std::uint64_t     wmPass       = 0;
        DirectX::XMFLOAT4 wmPrevRot{ 0.f, 0.f, 0.f, 1.f };
        DirectX::XMFLOAT3 wmPrevPos{ 0.f, 0.f, 0.f };
        float             wmPrevScale  = 1.f;
        DirectX::XMFLOAT4 wmDeltaRot{ 0.f, 0.f, 0.f, 1.f };
        DirectX::XMFLOAT3 wmDeltaPos{ 0.f, 0.f, 0.f };
        float             wmDeltaDt    = 0.f;
        // Passes left in THIS frame after one recorded the frame's own world
        // sample outright (a fresh record, a teleport): they see that same
        // sample, so they measure nothing (a "zero velocity" there would be a
        // fake start kick next frame). 0 outside a preview multi-step frame.
        std::uint32_t     wmSamePasses = 0;
    };

    // One pass's world-motion input for Simulate (PrepareWorldKick). The frame
    // delta D maps the PREVIOUS pass's skeleton space onto this one for a
    // world-fixed point, p' = p * R(rot) + pos (skeleton units); prev* is the
    // previous pass's D. A skeleton-fixed point at x moves relative to the
    // world at V(x) = (x - (x R + pos)) / dt - Simulate scales both deltas by
    // the strand's worldInertia* and re-expresses every simulated particle's
    // (and spring's) velocity so its WORLD velocity is conserved:
    //   v' = v R + (V_prev(x) R - V(x)).
    struct WorldKick
    {
        DirectX::XMFLOAT4 rot{ 0.f, 0.f, 0.f, 1.f }, prevRot{ 0.f, 0.f, 0.f, 1.f };
        DirectX::XMFLOAT3 pos{ 0.f, 0.f, 0.f },      prevPos{ 0.f, 0.f, 0.f };
        float             dt = 0.f,                  prevDt = 0.f;
    };

    // ---- Preview lane ----
    // One A/B pose feed (SetPreviewPoseFeed).
    struct PoseFeed
    {
        Entity shadow = NullEntity, source = NullEntity;
        std::vector<AnimationSystem::LocalPose> pose;   // source's input, this frame
    };

    // One entity's animated input pose for a multi-step frame (m_previewPoseSnap / m_previewPrevPose).
    struct PreviewPoseSnap
    {
        Entity e = NullEntity;
        std::uint64_t serial = 0;   // the m_updateSerial it was taken in
        std::vector<AnimationSystem::LocalPose> pose;
    };

    // ---- World-motion inertia ----
    // Serial phase 1 of RunPass: read `src`'s GlobalTransform, test the
    // teleport thresholds, record the transform and delta on `data`. True
    // (and `out` filled) when this pass must kick: a contiguous, non-teleport
    // step with a previous delta to difference against.
    bool PrepareWorldKick(World& world, Entity e, Entity src, EntityData& data,
                          const ChainPhysicsComponent& cfg, float dt, Lane lane,
                          WorldKick& out);

    // ---- Preview lane: HOLD and multi-step frames ----
    // HoldPass: RunPass's serial phase-1 walk over @p set WITHOUT InitEntity or integration -
    // ApplyRestPose (authored data, as every pass), the no-clip resetRot over the solver's write range
    // (skipping IsRestPosedBone like RunPass), then WriteBonePoses / WriteSpringBonePoses from the
    // EXISTING particle / spring state, island by island. Given the same input pose it reproduces the
    // last pass's output bit for bit. Serial, preview lane only, never nested in a ParallelFor; an
    // entity with no simulation state yet (just reset / never stepped) gets only the authored rest
    // pose, as RunPass does before its InitEntity.
    void HoldPass(World& world, const std::unordered_set<Entity>& set);
    // Multi-step frames: the frame's animated input pose of every preview chain
    // entity, taken before pass 1 (every stepping or holding frame - it is the
    // NEXT frame's interpolation origin) and written back before passes 2..n.
    void SnapshotPreviewPoses(World& world, const std::unordered_set<Entity>& set);
    void RestorePreviewPoses();
    // Pass k of n (frac = k/n < 1): this frame's input interpolated from the
    // previous preview frame's (valid only if that was the IMMEDIATELY
    // preceding Update); an entity without one gets this frame's input.
    // Returns how many entities were interpolated.
    std::uint32_t WriteInterpolatedPreviewPoses(float frac);
    // End of a stepping / holding frame: this frame's input becomes the next
    // frame's interpolation origin.
    void KeepPreviewInputAsPrevious();
    void DropPreviewPrevPose(Entity e);

    // ---- Preview lane: solo / poke / the A/B pose feed ----
    // SERIAL, preview lane only (phase 1 of a Lane::Preview RunPass, HoldPass, the pose snapshots):
    // none is reachable from the main lane.
    PoseFeed* FindPoseFeed(Entity shadow);
    const PoseFeed* FindPoseFeed(Entity shadow) const;
    // The entity whose skeleton / GlobalTransform / capsules / pose-source
    // verdict a preview-lane entity uses: its feed's source, else itself.
    Entity PreviewPoseSourceOf(Entity e) const;
    // The LocalPose a preview-lane pass reads and writes for `e`: the feed's
    // own buffer for a shadow (nullptr until fed), else AnimationSystem's.
    AnimationSystem::LocalPose* PreviewPoses(Entity e);
    const AnimationSystem::LocalPose* PreviewPosesConst(Entity e) const;
    // Start of the preview lane: every shadow's buffer = its source's input.
    void FeedPreviewPoses(World& world);
    // Right after InitEntity on the preview lane: apply m_soloGroups[e].
    void ApplySoloFreeze(Entity e, EntityData& data);
    // Phase 1 of a preview pass: resolve the pending pokes into extAccel for a
    // pass of `dt` and spend their TTL. True when any acceleration is active.
    bool PrepareExternalImpulses(EntityData& data, float dt);
    // OnEntityDestroyed's part (not a pass): erase @p e's solo entry and every pose feed naming it
    // (as shadow or source).
    void DropPreviewM7(Entity e);

    // ---- Authored rest pose (Edit / Pause path) ----
    // Apply the authored rest pose and nothing else: the same pool walk and six guards as Update's
    // phase 1, but no InitEntity, no EntityData, no integration. @p exclude (the preview set, on the
    // MAIN lane) is skipped like a culled entity - still VISITED, so the orphan sweep at the end keeps
    // its record.
    void ApplyRestPoseOnly(World& world, const std::unordered_set<Entity>* activeSet,
                           const std::unordered_set<Entity>* exclude = nullptr);

    // ---- Chain detection ----
    void InitEntity(Entity e, const ChainPhysicsComponent& cfg,
                    const SkeletonAsset& skel,
                    const AnimationSystem::LocalPose* poses);

    // DFS from 'root' through physics children. At forks, each branch becomes
    // its own chain (shared ancestors are duplicated as the root prefix).
    // Appends all discovered linear chains to 'outChains'.
    void TraceChains(uint32_t root, const SkeletonAsset& skel,
                     const std::vector<bool>& isPhysBone,
                     const std::vector<std::vector<uint32_t>>& children,
                     std::vector<uint32_t>& current,
                     std::vector<std::vector<uint32_t>>& outChains);

    // The skeleton-space position of @p bone, by FK over @p poses.
    DirectX::XMFLOAT3 ComputeBindWorldPos(uint32_t bone,
                                           const SkeletonAsset& skel,
                                           const AnimationSystem::LocalPose* poses);

    // ---- Simulation ----
    // Partition strands into data-disjoint islands, compute their contiguous
    // particle/constraint ranges, and merge islands whose pose FK reads cross
    // another island's written bones into one serial task. Called by InitEntity.
    void BuildIslands(EntityData& data, const SkeletonAsset& skel);

    // Serial pre-pass (Update phase 1): compute this frame's substep count for an entity (authored
    // minimum, raised by the capsule-motion CCD) and apply the Verlet displacement rescale when the step
    // duration changes. Must run before the parallel island jobs: it touches entity-wide state.
    // @p subCap caps the CCD substep raise for a LOD tier. It is applied INSIDE, before the velocity
    // rescale: clamping the returned value instead would desync data.velocityStepSeconds from what
    // Simulate uses, and the next frame's stepRatio would kick every chain once per tier change.
    int PrepareSubsteps(EntityData& data, const ChainPhysicsComponent& cfg,
                        const WorldCapsule* capsules, int capsuleCount, float dt,
                        int subCap = 8);

    // Simulate ONE island of an entity. Islands of one entity are data-disjoint (BuildIslands), so they
    // may run as concurrent TaskSystem jobs. Returns true when the divergence safeguard clamped a
    // runaway particle (the caller logs once per entity, outside the parallel region).
    // @p iterations / @p collideAllowed come from the LOD tier, NOT from cfg, so the Inspector and the
    // serialized asset never see a runtime quality decision; collideAllowed only REMOVES collision
    // (AND-ed with the authored per-group flag), never grants it.
    // @p swayTime: the ambient-sway / wind phase of the lane running this pass, by value - Simulate runs
    // on phase-2 workers and reads no clock member.
    // @p worldKick (nullable): this pass's world-motion input (PrepareWorldKick); null = no kick.
    bool Simulate(EntityData& data, const Island& isl,
                  const ChainPhysicsComponent& cfg, float dt, int nSub,
                  int iterations, bool collideAllowed,
                  AnimationSystem::LocalPose* poses, const SkeletonAsset& skel,
                  const WorldCapsule* capsules, int capsuleCount,
                  float swayTime,
                  const DirectX::XMFLOAT3* gravityDown = nullptr,
                  const WorldKick* worldKick = nullptr);

    // Push the island's particles/links out of capsule colliders. The swept
    // pass is used once after integration; static passes are interleaved with
    // PBD distance iterations so constraints cannot pull cloth back through the
    // body. capsulesPrev (nullable) = the SAME capsules at the start of this
    // substep; when given, the tunneling side test measures the particle's
    // entry side against it, so a fast collider sweeping past a slow particle
    // is detected.
    // skin = the multiplicative contact-radius margin (ChainPhysicsComponent::
    // contactSkin, already clamped to >= 1 by the caller). Passed rather than
    // read from the config because this function is island-scoped and takes no
    // config; the per-group half of the contact material rides on
    // Particle::friction, baked once per frame in Simulate.
    void SolveCapsuleCollision(EntityData& data, const Island& isl,
                               const WorldCapsule* capsules, int capsuleCount,
                               const WorldCapsule* capsulesPrev,
                               bool sweepParticles, bool applyFriction,
                               float skin);

    // Clamp every bone's simulated direction into a cone around its ANIMATED rest direction
    // (KawaiiPhysics LimitAngle). Rotates the parent->child offset instead of translating the particle,
    // so the segment length (and the PBD distance constraint around it) is preserved exactly; runs
    // root->tip so a clamped parent is final before its child is measured.
    // INTERLEAVED with the PBD iterations like SolveCapsuleCollision: one clamp after the whole solve is
    // a positional edit the horizontal / shear constraints never re-converge around (a kinked cloth
    // piece, a cone that reads as a hard wall).
    // @p subAlpha selects the substep's interpolated animation pose - the one the kinematic roots, the
    // colliders and the local pull use, so the cone never leads or lags them. @p applyVelocityResponse
    // must be true only on the LAST call of the frame: cancelling the outward velocity per PBD
    // iteration would scale that damping by cfg.iterations.
    // @p capsules (nullable): the substep's colliders. A cone whose boundary point lies INSIDE a
    // capsule's contact field cannot satisfy both constraints (snap + eject every iteration oscillates
    // with no fixed point), so such segments skip the clamp for this call (collision owns them) until
    // the animated rest direction leaves the collider.
    void ApplyRestLimit(EntityData& data, const Island& isl,
                        const ChainPhysicsComponent& cfg,
                        const SkeletonAsset& skel,
                        float subAlpha, bool applyVelocityResponse,
                        const WorldCapsule* capsules = nullptr,
                        int capsuleCount = 0, float skin = 1.f);

    // Verlet integration for a single strand.
    void IntegrateStrand(EntityData& data, const Strand& strand, float dt,
                         float damping, const DirectX::XMFLOAT3& gravity,
                         float maxVelocity);

    // Write the island's simulated particle positions back to bone local poses.
    void WriteBonePoses(EntityData& data, const Island& isl,
                        AnimationSystem::LocalPose* poses,
                        const SkeletonAsset& skel);

    // ---- Spring bone simulation ----
    // @p gravityDown: the preview scene's gravity vector (SetPreviewGravity); nullptr = the authored
    // Y-only path.
    // @p worldKick (nullable): see Simulate - applied to every non-frozen spring (virtual roots
    // included) before the spring-damper step.
    void SimulateSpringBones(EntityData& data, const ChainPhysicsComponent& cfg,
                             float dt, AnimationSystem::LocalPose* poses,
                             const SkeletonAsset& skel,
                             const DirectX::XMFLOAT3* gravityDown = nullptr,
                             const WorldKick* worldKick = nullptr);

    void WriteSpringBonePoses(EntityData& data,
                              AnimationSystem::LocalPose* poses,
                              const SkeletonAsset& skel);

    // FK transform of @p boneIndex over @p poses, in skeleton space (same logic as IKSystem).
    DirectX::XMMATRIX ComputeWorldTransform(uint32_t boneIndex,
                                             const AnimationSystem::LocalPose* poses,
                                             const SkeletonAsset& skel) const;

public:
    // ---- Distance LOD ----
    // Chain physics is ~90% of the animation phase at 100 characters; activeSet is only a binary
    // visible gate, and the solver's adaptive knobs (CCD substep raise, swept-march samples) only go UP.
    // Tiers reduce QUALITY, never RATE. Do not solve every Nth frame with dt x N:
    //   * Simulate clamps dt to 1/30, so a 3x step silently drops a third of the elapsed time;
    //   * duration-aware Verlet history does not recover the collider trajectories or contact
    //     convergence of skipped frames;
    //   * it costs more: prevCapsules is written only for entities that ran, so after two skipped
    //     frames PrepareSubsteps sees three frames of hip travel and saturates nSub at 8 (41 solver
    //     passes against 11, 13.7 amortised per frame) - a moving character gets slower and worse.
    // Tier 2 is the win: it turns collision off (~80% of the solver); the authored limit cone
    // (limitAngle / ovrLimitAngle) keeps a far skirt in silhouette without it.
    LodTiers lodTiers{};

    // OFF by default, for the same reason AILODSystem::enabled exists: a
    // per-frame quality rewrite silently invalidates every measurement taken
    // over it. Turn it on for gameplay, off to benchmark the solver.
    bool lodEnabled = false;

private:
    DirectX::XMFLOAT3 m_cameraPos{ 0.f, 0.f, 0.f };
    const std::unordered_set<Entity>* m_shadowOnly = nullptr;

    // ---- Authored rest pose (Edit / Pause path) ----
    // Bones this entity's authored rest pose DROVE last frame, for the Edit-path restore. Play does not
    // need it (resetRot self-heals), but the Edit path never runs resetRot and AnimationSystem seeds a
    // source-less skeleton only once, so un-ticking Enabled / [Clear all] / deleting an entry would
    // leave the last written rotation in poses[b].rot forever (the A/B toggle a one-way valve). Bones
    // driven last frame but not this frame are restored to skel.restPoseLocal[b].
    std::unordered_map<Entity, std::vector<uint32_t>> m_editRestPoseApplied;

    // ---- Members ----
    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;

    std::unordered_map<Entity, EntityData> m_entityData;
    bool m_simulationEnabled = true; // Game/ShaderLab default; Editor overrides per mode.
    bool m_hold = false;             // SetHold

    // Ambient-sway clock of the MAIN lane. Advanced once per frame in Update's
    // serial section (only when the main transport runs) and handed to RunPass
    // as its swayTime argument; the parallel island jobs only ever read that
    // copy, so no synchronization is needed.
    float m_time = 0.f;

    // ---- Preview lane (see SetPreviewLane) ----
    const std::unordered_set<Entity>* m_previewSet = nullptr;   // caller-owned
    float    m_previewDt   = 0.f;
    // The PREVIEW lane's own sway phase, not m_time: in Edit mode the main lane never advances
    // (m_simulationEnabled false AND dt 0), so a shared phase would freeze wind / ambient sway into a
    // static lean.
    float    m_previewTime = 0.f;
    PreviewLaneStats m_laneStats{};
    std::uint32_t    m_previewLogEvery = 0;   // periodic lane line every n frames, harness-only; 0 = never
    // The preview scene (SetPreviewGravity / SetPreviewSceneCapsules). Written only by those setters
    // (the panel's serial window) and ClearAll; read by RunPass's phase 1 / prepare for Lane::Preview
    // jobs only.
    bool             m_previewGravityActive = false;
    DirectX::XMFLOAT3 m_previewGravityDown  = { 0.f, -1.f, 0.f };
    WorldCapsule     m_previewSceneCaps[kMaxPreviewSceneCapsules] = {};
    std::uint32_t    m_previewSceneCapCount = 0;
    std::uint32_t    m_previewSteps = 1;   // passes this frame (see SetPreviewLane)

    // The per-entity input-pose snapshot a multi-step frame restores between passes. The vectors are
    // reused across frames (no per-frame allocation once grown to the stage's size).
    std::vector<PreviewPoseSnap> m_previewPoseSnap;       // this frame's input
    std::size_t                  m_previewPoseSnapCount = 0;
    std::vector<PreviewPoseSnap> m_previewPrevPose;       // the previous preview frame's
    std::size_t                  m_previewPrevCount = 0;
    std::vector<std::pair<std::uint32_t, DirectX::XMFLOAT4>> m_holdScratch;   // HoldPass diff
    // Which entities each lane pushed into its jobs (sorted), and in which
    // Update (m_updateSerial) - solvedBothLanes counts the overlap of the two
    // when both ran in one frame. Recorded only while a preview set is
    // installed, so the main lane pays one pointer test otherwise.
    std::vector<Entity> m_laneSolved[2];
    std::uint64_t       m_laneSolvedSerial[2] = { ~0ull, ~0ull };
    std::uint64_t       m_updateSerial = 0;
    std::uint64_t       m_rateHolds    = 0;   // half-rate holds since the last [CHAINLOD] line
    // Preview-set entities whose Edit-path rest-pose record was created once; a SECOND creation is
    // churn (restPoseRecordChurn). Erased with the entity (OnEntityDestroyed) and by ResetPreviewEntity.
    std::unordered_set<Entity> m_previewRecordSeen;
    // The per-entity solo (SetSoloGroup) and the A/B pose feeds (SetPreviewPoseFeed). Written only by
    // those setters (the panel's serial window), OnEntityDestroyed, ClearAll and the lane hand-back;
    // read by Lane::Preview passes only.
    std::unordered_map<Entity, int> m_soloGroups;
    std::vector<PoseFeed>           m_poseFeeds;

    // ---- World-motion inertia ----
    // RunPass calls per lane (the contiguity clock of EntityData::wmPass).
    std::uint64_t m_lanePasses[2] = { 0, 0 };
    // Passes left in this frame INCLUDING the one about to run (Update's
    // preview multi-step loop: n - k). A pass moves its entity transform
    // 1/m_passRemaining of the way to this frame's, so an n-step frame spreads
    // its world motion over its n passes the way it spreads the clip pose.
    // 1 everywhere else (the main lane, a direct RunPass call).
    std::uint32_t m_passRemaining = 1;
    std::uint64_t m_worldKicksTotal = 0, m_worldTeleportsTotal = 0;
    std::uint32_t m_worldTeleportLogs = 0;   // the per-teleport line is capped
    bool          m_worldInertiaSuppressed = false;
};
