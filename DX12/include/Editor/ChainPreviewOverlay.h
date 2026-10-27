#pragma once

// ChainPreviewOverlay - the Chain Physics Preview panel's debug overlay: a CPU
// PROJECTION into the panel's own ImDrawList, over the preview image. No second
// DebugWirePass, no GPU work, no render-seam crossing (the render half never
// reads World).
//
// TWO HALVES, the preview layer's rule:
//   * BuildSnapshot runs in the panel's ProcessPendingActions (the game half's
//     SERIAL window) and is the only half that reads the World / the solver:
//     the bones' FINAL pose through EditorLayer::BuildBoneWorldMatrices (FK over
//     the post-chain LocalPose; the solver's boneWorldMatCache is its
//     PRE-simulation target, so it is deliberately not the source), the panel's
//     STAGED ChainPhysicsComponent (what the tree edits; equal to the live one
//     after the panel's sync) and the stage's CapsuleColliderComponents, all
//     copied into a Snapshot of world-space points.
//   * Draw runs in the panel's Draw (the UI half) and reads ONLY the Snapshot.
//
// PROJECTION = ExtractPreviewCameras' convention exactly (Renderer
// MakeSecondaryViewProj): XMMatrixLookToLH(eye, forward, up) x
// XMMatrixPerspectiveFovLH(fovY, aspect, FAR, NEAR) - reverse-Z swaps the two
// depth arguments. The aspect and the NDC -> pixel mapping use the preview's
// OUTPUT size (Renderer::GetPreviewOutputSize), never ImGui's avail size: the
// two disagree between a resize and the lane recompile. The panel authored the
// camera, so it rebuilds the matrix from the eye / forward / up / lens it wrote
// instead of borrowing the renderer's private helper.
//
// No occlusion; "Depth fade" stands in: primitives are painter-sorted far to
// near and their alpha falls off with camera distance.
// TODO(ChainPhysics_Preview_Viewport_Plan M9): a depth-tested preview wire pass.

#include "ECS/ECS.h"
#include "Physics/ChainPhysicsSystem.h"   // ChainPhysicsSystem::DebugSnapshot

#include <DirectXMath.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class Renderer;
class World;
struct ChainPhysicsComponent;
struct ImDrawList;
struct ImVec2;

namespace Editor
{
namespace ChainOverlay
{
    // Show-flag bits. The panel keeps TWO masks (like PhAT's MeshViewMode vs
    // MeshViewModeSim): m_showEdit while the simulation is off, m_showSim while
    // it runs.
    enum Bits : std::uint32_t
    {
        kRoots     = 1u << 0,   // group root spheres (gold; white when focused)
        kChain     = 1u << 1,   // simulated chain bones (cyan; orange when focused)
        kLinks     = 1u << 2,   // parent links between simulated bones (green / amber)
        kExcludes  = 1u << 3,   // excluded bones (red X)
        kRestPose  = 1u << 4,   // bones with an enabled authored rest rotation (magenta ring)
        kCone      = 1u << 5,   // the focused group's rest-limit cones
        kCapsules  = 1u << 6,   // the chain solver's capsule colliders (wireframe)
        // ---- the SOLVER's state (FillDebugSnapshot; drawn only while it has one) ----
        kParticles   = 1u << 7,   // particles + their collision radius (Particle::radius)
        kConstraints = 1u << 8,   // constraint links by bucket: vertical / ring / shear
        kIslands     = 1u << 9,   // particles coloured by simulation island
        kLimits      = 1u << 10,  // particles ON (ring) / BEYOND (disc) their rest-limit cone, red
        kOwnership   = 1u << 11,  // writesBone: the segment whose direction WRITES each bone
        kLodSkip     = 1u << 12,  // group roots whose collision the LOD tier switched off (grey X)
        kInterpen    = 1u << 13,  // capsules interpenetrating another capsule, red
        // ---- A/B ghost ----
        // Parameter set B's particles + links (its own FillDebugSnapshot), desaturated,
        // mapped with A's skeleton -> world transform (B is solved on A's pose).
        // Drawn only while A/B is on.
        kGhost       = 1u << 14,
        kBitCount  = 15,
        kAll         = (1u << kBitCount) - 1u,
        // The first seven bits (bones, cones, capsules): the solver bits describe a
        // simulation, and there is none while editing.
        kDefaultEdit = (1u << 7) - 1u,
        kDefaultSim  = kRoots | kChain | kLinks | kCapsules | kParticles | kConstraints |
                       kLimits | kLodSkip | kInterpen | kGhost,
        kSimBits     = kParticles | kConstraints | kIslands | kLimits | kOwnership |
                       kLodSkip | kInterpen,
    };
    const char* BitName(std::uint32_t bitIndex);

    // What the game half copied for the UI half. World space.
    struct Snapshot
    {
        struct Capsule { DirectX::XMFLOAT3 a, b; float radius; };

        struct Sim
        {
            bool                                  valid = false;
            std::vector<DirectX::XMFLOAT3>        pos;          // per particle, world
            std::vector<float>                    radius;       // world units
            std::vector<std::uint8_t>             island, limit, pinned;
            std::vector<std::uint8_t>             owner;        // 1 = writes its bone (segment i -> i+1)
            std::vector<std::pair<std::uint32_t, std::uint32_t>> links;
            std::vector<std::uint8_t>             bucket;       // 0..5 (0-1 vertical, 2-3 ring, 4-5 shear)
            std::vector<DirectX::XMFLOAT3>        lodSkipRoots; // LOD-skipped collide groups' roots
            std::vector<Capsule>                  interCaps;    // interpenetrating capsules, world
            std::vector<std::uint8_t>             frozen;       // frozen at rest by a solo
        };

        struct Ghost
        {
            bool                                  valid = false;
            std::vector<DirectX::XMFLOAT3>        pos;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> links;
        };

        bool                           valid = false;
        std::vector<DirectX::XMFLOAT3> bone;        // per skeleton bone
        std::vector<std::int32_t>      parent;
        // 0 = not a chain bone, 1 = group root, 2 = simulated chain bone,
        // 3 = excluded (inside a group's subtree, dropped by its exclude list)
        std::vector<std::uint8_t>      kind;
        std::vector<std::uint8_t>      group;       // owning group, 0xFF none
        std::vector<std::uint8_t>      linkToParent;
        std::vector<std::uint8_t>      restPosed;   // an ENABLED authored rest rotation
        int                            focusGroup = -1;   // the highlighted group (-1 none)
        // the focused group's rest-limit cones, as world segments
        std::vector<std::pair<DirectX::XMFLOAT3, DirectX::XMFLOAT3>> cone;

        std::vector<Capsule>           capsules;

        // The solver's own state (FillDebugSnapshot), mapped from SKELETON space (the
        // solver simulates there) into the world with the SAME skeleton -> world
        // transform the bones above use. valid = false while the chains have no state
        // (a reset, the Edit-mode rest pose).
        Sim                            sim;

        // The A/B ghost (parameter set B's solver snapshot), world space through the
        // SAME transform as `sim`.
        Ghost                          ghost;
    };

    // The stats overlay: a few text lines in the viewport's lower-left corner.
    // The panel formats them in its serial window (Draw reads no solver); line 0
    // (the solve cost) is drawn larger.
    struct StatsBox
    {
        bool                     show = false;
        std::vector<std::string> lines;
    };

    // The camera the panel authored into its camera entity this frame.
    struct Camera
    {
        DirectX::XMFLOAT3 eye{ 0.f, 0.f, 0.f };
        DirectX::XMFLOAT3 forward{ 0.f, 0.f, 1.f };
        DirectX::XMFLOAT3 up{ 0.f, 1.f, 0.f };
        float fovY = 0.6f, nearZ = 0.05f, farZ = 200.f;
        bool  valid = false;
    };

    struct Stats
    {
        std::uint32_t bones = 0;          // bone markers (roots + chain + excludes) drawn
        std::uint32_t links = 0;
        std::uint32_t capsules = 0;
        std::uint32_t excludes = 0;
        std::uint32_t restPosed = 0;
        std::uint32_t coneSegs = 0;
        // Of the bone markers, how many project OUTSIDE the view: behind the
        // camera (w <= 0), |NDC x| or |NDC y| > 1, or depth outside [0, 1].
        // A projection-correctness proxy: a framed subject reads 0, a wrong view /
        // lens convention does not.
        std::uint32_t offscreenBones = 0;
        std::uint32_t prims = 0;          // primitives emitted after culling
        // solver-state marks drawn
        std::uint32_t particles = 0, constraints = 0, limitMarks = 0, ownerLines = 0;
        std::uint32_t lodMarks = 0, interCaps = 0, statsLines = 0;
        // The A/B ghost's projected particles / links, and the particles drawn as
        // frozen by a solo.
        std::uint32_t ghostParticles = 0, ghostLinks = 0, frozenMarks = 0;
    };

    // FK over the post-chain pose (EditorLayer::BuildBoneWorldMatrices); the
    // overlay must not derive FK a second time.
    using BoneFn = std::function<bool(Renderer&, World&, Entity,
                                      std::vector<DirectX::XMFLOAT4X4>&,
                                      DirectX::XMMATRIX&)>;

    // Game half, serial. `chain` is the stage's chain carrier, `cfg` the
    // authored parameters to draw (the panel's staged copy), `stage` the stage
    // entity set (capsule colliders are searched there). focusGroup < 0 = auto:
    // the first enabled Chain / Cloth group with a rest-limit angle.
    // `dbg` (nullable) = the solver snapshot of `chain` (FillDebugSnapshot); it
    // fills out.sim. `ghost` (nullable) = the A/B shadow's snapshot (solved in the
    // same skeleton space as `chain`); it fills out.ghost.
    bool BuildSnapshot(Renderer& renderer, World& world, Entity chain,
                       const ChainPhysicsComponent& cfg,
                       const std::unordered_set<Entity>& stage, int focusGroup,
                       const BoneFn& bones, Snapshot& out,
                       const ChainPhysicsSystem::DebugSnapshot* dbg = nullptr,
                       const ChainPhysicsSystem::DebugSnapshot* ghost = nullptr);
    // The skeleton -> world transform BuildSnapshot maps the solver's state with
    // (the same `bones` call, so the poke's ray lands in exactly the space the
    // overlay draws). False when it cannot be built.
    bool SkeletonToWorld(Renderer& renderer, World& world, Entity chain,
                         const BoneFn& bones, DirectX::XMFLOAT4X4& out);

    // badProj = the harness's reverse arm (negative control): the view looks AWAY
    // from the subject (forward negated), so every bone is behind the camera.
    DirectX::XMMATRIX ViewProj(const Camera& cam, float aspect, bool badProj);
    // The same two factors, separately (ImGuizmo's input).
    void ViewAndProj(const Camera& cam, float aspect, bool badProj,
                     DirectX::XMMATRIX& outView, DirectX::XMMATRIX& outProj);

    // UI half. rectMin / rectSize = the pixels the preview's OUTPUT occupies.
    Stats Draw(ImDrawList* dl, const ImVec2& rectMin, const ImVec2& rectSize,
               const DirectX::XMMATRIX& viewProj, float fovY, const Snapshot& snap,
               std::uint32_t mask, bool depthFade, const StatsBox* stats = nullptr);

    // Harness: draw the overlay alone into a PRIVATE ImDrawList sized to the
    // preview output and software-rasterize its triangles (per-vertex colour,
    // alpha-blended over opaque black) into a PNG - the overlay LAYER, exactly
    // the vertices ImGui would submit (ImGui's own capture paths and
    // -preview_screenshot never see the panel's draw list). Returns the number
    // of pixels the layer touched; < 0 on failure (err says why).
    long long WriteLayerPng(const std::string& path, std::uint32_t w, std::uint32_t h,
                            const DirectX::XMMATRIX& viewProj, float fovY,
                            const Snapshot& snap, std::uint32_t mask, bool depthFade,
                            Stats* outStats, std::uint32_t* outTris, std::string* err,
                            const StatsBox* stats = nullptr);
}
}
