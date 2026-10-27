#pragma once

// PreviewTypes - the value types that cross the material-preview seam.
//
// Everything here is POD on purpose: the editor's ImGui pass latches a request,
// the GAME half hands it over (ExtractRequests, end of BuildRenderScene) to the
// RENDER half, which records it, and the result travels back to the game half's
// ApplyFeedback. Both directions are plain non-buffered members on the Renderer,
// the same shape as the reflection-probe bake lane (ProbeBakeRequest /
// m_probeBakeDone). Safety comes from park-first: the game half opens with
// FramePipeline::RenderPhaseBarrier, which drains the render half, so render half F has
// joined before game frame F+1 touches these.
//
// The latch itself is written from OnUIRender, which runs under
// a Render ThreadCtx in FramePipeline::BuildEarlyEditorUI or FramePipeline::RenderHalf -
// the editor UI half, not the game half.
// That is safe only because the render half is parked meanwhile (see the threading block
// in PreviewRenderer.h); a Game-side PiP must go through the game half instead.
//
// Request carries a full MaterialComponent value copy plus the content-addressed
// runtime key that names its textures. The copy is required, not defensive: the
// record half runs under ThreadCtx::Render and may not read World, and a
// field-by-field snapshot that misses one schema field is a silent wrong picture
// (see Renderer::WriteMaterialSlot).

#include "ECS/Components.h"        // MaterialComponent - carried BY VALUE, see Request
#include "ECS/HierarchyComponents.h"  // ViewBit - the secondary view claims one
#include "Graphics/FrameConstants.h"  // RHI::kFramesInFlight - the idle-teardown floor
#include "Graphics/GpuLight.h"        // GpuShared::GPULight - the clustered-light record
#include "Graphics/GraphicsStruct.h"  // RHI::Texture - the thumbnail tile the editor owns
#include "Graphics/RenderTypes.h"     // RenderCamera - the WorldCull view's camera
#include "Resource/PrefabSerializer.h"  // PrefabSkipCounts - the declined-node tally

#include <cstdint>
#include <string>
#include <vector>

namespace Preview
{
    // How many draw packets one Synthetic view may emit. A hard cap on three private
    // rings at once - the instance table (kMaxPreviewParts x GPUInstanceData), the
    // material table (1 + kMaxPreviewParts records; slot 0 is the engine-reserved
    // neutral one) and the custom-shader net (one 256 B b8 slice and one eight-slot
    // space3 block per part per frame in flight) - so it is a VRAM and descriptor
    // budget:
    //   instances  64 x 176 B x 3 frames                =  33 KiB
    //   materials  65 x sizeof(MaterialGPUData) x 3     = ~180 KiB at 944 B
    //   b8 CBV     64 x 256 B x 3                       =  48 KiB
    //   space3     64 x 8 x 3 = 1536 descriptors of the 1,000,000-slot heap
    // Over the cap the resolver keeps the first kMaxPreviewParts parts, logs once and
    // reports the overflow to the panel; it never silently renders a subset that looks
    // complete (a character missing its hair reads as a broken import). 64 fits every
    // shipped asset with room (the widest prefab has 37 parts, the widest .meshlib 35
    // entries); a level-sized .iscn is not something a preview tile can usefully show.
    constexpr uint32_t kMaxPreviewParts = 64;

    // Which kind of asset a Synthetic view is showing.
    //   None      - the single-primitive material preview.
    //   Mesh      - a `.meshlib`: one packet per library entry, materials taken from
    //               the sibling `.iscn`'s F-lines when there is one.
    //   Prefab    - a `.ipfb`: its node tree composed on the GAME half into a flat
    //               list of {mesh, material, world matrix} (Synthetic multi-packet,
    //               not a private World).
    //   Model     - a `.iscn`: its N/F lines composed exactly as a prefab's SceneRef
    //               node composes them, at the identity - the model as placing it
    //               would show it.
    //   MeshAsset - one Mesh of a model, named "<model .iscn>#<MeshKey hex>"
    //               (Resource::MeshAssetRef): its sections at the identity, in the
    //               mesh's own space, with the materials the model gives them. A
    //               `.meshlib` tile stacks a level import's meshes at the origin;
    //               this is the picture of one of them.
    enum class AssetKind : uint8_t { None = 0, Mesh = 1, Prefab = 2, Model = 3, MeshAsset = 4 };

    // The kind an asset path previews as: a MeshAsset ref, then the extension
    // (.meshlib / .ipfb / .iscn), else None. The ONE mapping - the browser, the
    // Asset Preview panel and the composer all ask this.
    AssetKind AssetKindForPath(const std::string& path);

    // One entry of an asset's composition as read off disk, before any mesh slot,
    // material component or shader id is resolved. This is the cacheable half (a pure
    // function of the files) that Renderer::m_previewAssetPlacements memoises per
    // path; AssetPart is the per-frame resolved half, not cacheable because a mesh
    // library streams and a material runtime is refcounted.
    struct AssetPlacement
    {
        // ---- Geometry: a library entry or a primitive ----
        // `primitiveId >= 0` is the discriminant. Two fields rather than a union because
        // they resolve through different machinery: a library entry is a (path, id) pair
        // that must be loaded (refcount, LRU, retry clock, mesh-descriptor registration),
        // while a primitive indexes MeshManager's five persistent GPUMesh slots (uploaded,
        // never evicted, no path). A `.ipfb` places geometry both ways: a SceneRef expands
        // into library meshes, and a `MeshHandle: gpuMeshID=N` node is what the editor
        // writes for a procedural primitive. A prefab with no renderable node of either
        // kind exits 12.
        std::string meshPath;         // project-relative `.meshlib`; "" for a primitive
        uint32_t    meshId = 0;
        // The F-line's `sec=` (0 = none). The resolve looks the entry up by it in the
        // loaded library; meshId is then only a hint (a re-imported, reordered library
        // leaves it stale).
        uint64_t    sectionKey = 0;
        // Resource::PrimitiveMeshType (0 Cube, 1 Sphere, 2 Cone, 3 Plane,
        // 4 Torus), mirroring Resource::PrefabNodeView::gpuMeshId. -1 = this is
        // a library placement. Range-checked where it is READ rather than
        // trusted from the file.
        int         primitiveId = -1;
        std::string matPath;          // project-relative .imat; "" = engine default
        // ---- The prefab's three material layers ----
        //   matPath          the .iscn F-line's `mat=`, or the node's MaterialRef
        //   matInlineText    a MaterialInline block - a whole material text that
        //                    REPLACES the above (the node has no source path)
        //   matOverride      a MaterialOverride property-bag diff, applied on top
        // Replayed in LoadPrefab's order: ref -> inline -> override.
        std::string      matInlineText;
        MaterialOverride matOverride;
        bool             hasMatOverride = false;
        float       world[16] = { 1.f, 0.f, 0.f, 0.f,
                                  0.f, 1.f, 0.f, 0.f,
                                  0.f, 0.f, 1.f, 0.f,
                                  0.f, 0.f, 0.f, 1.f };
        // ---- Provenance, for the order self-check only ----
        // Nothing in the draw path reads these two; `-preview_asset_verify` does.
        // `sceneNodeIdx` is the prefab node whose SceneRef expanded into this part (-1 for
        // a bare `.meshlib`, which has no node tree), and `dfsIndex` is the part's
        // position inside the subtree that SceneRef spawns - the only key a prefab's
        // per-part material overrides are applied by, so the number a self-check compares.
        int         sceneNodeIdx = -1;
        uint32_t    dfsIndex     = 0;
    };

    // Everything one asset's files say, memoised together. `sourceFiles` is the
    // dependency set a disk-cached thumbnail is stamped against (the asset, every
    // `.iscn` a prefab expands and every `.meshlib` a part comes from); it is collected
    // here because the editor cannot see which scenes a prefab pulled in without
    // re-parsing it.
    struct AssetComposition
    {
        std::vector<AssetPlacement> placements;
        std::vector<std::string>    sourceFiles;
        // Node kinds the prefab walk declined (emitters, trails, lights, decals,
        // billboards). Memoised with the composition, so the tally is paid once per asset
        // path; the panel shows it beside the part count so a picture that is partial by
        // design reads as partial, not wrong. Always zero for a bare `.meshlib`.
        Resource::PrefabSkipCounts  skipped{};
    };

    // One resolved draw of a multi-packet asset preview, resolved on the GAME half by
    // Renderer::ResolvePreviewAsset: MeshLibrary::Load and
    // MeshManager::RegisterMeshLibMesh allocate descriptors,
    // ShaderLibrary::RegisterDynamic reallocates a vector a compiling worker may hold a
    // pointer into, and the record half runs under ThreadCtx::Render and may not read
    // World or the asset database.
    struct AssetPart
    {
        uint32_t meshDescSlot = 0xFFFFFFFFu;
        uint32_t indexCount   = 0;
        // ROW-MAJOR, UN-TRANSPOSED, and already carrying the framing fit. The
        // record half transposes it into GPUInstanceData exactly as it does the
        // single-part identity, so the transpose lives in one place.
        float    world[16] = { 1.f, 0.f, 0.f, 0.f,
                               0.f, 1.f, 0.f, 0.f,
                               0.f, 0.f, 1.f, 0.f,
                               0.f, 0.f, 0.f, 1.f };
        // The part's AABB CENTRE in the same (fitted) space as `world`, so the
        // record half can order the depth-sorted filters back to front without
        // re-deriving a box it does not have. The matrix TRANSLATION is not a
        // substitute: a level import places every part at the same node
        // transform and separates them by their geometry alone, which would
        // collapse every depth key onto one value.
        float    centre[3] = { 0.f, 0.f, 0.f };
        // The material this part draws with, by VALUE - the record half runs the
        // same Renderer::WriteMaterialSlot the main path runs, so the record is
        // byte-identical by construction rather than by review.
        MaterialComponent mat;
        // BuildMaterialRuntimeKey(mat): names the shared RuntimeMaterialResource that
        // owns this material's textures. Re-resolved by key every frame on the record half
        // and never cached.
        uint64_t matRuntimeKey = 0;
        uint32_t customPSID    = 0;
        uint32_t customVSID    = 0;
        bool     texturesReady = false;
        // Which placement of the composition this part came from (a dropped
        // placement leaves a gap, so this is NOT the part's position in the
        // vector). ViewDesc::partMask / highlightPart are keyed on it.
        uint32_t placementIndex = 0;
    };
    // The one thumbnail size. Tiles and the thumbnail graph are both exactly this
    // size because CopyTextureSubresource copies a whole subresource with no scaling
    // (pSrcBox = nullptr), which removes any sub-rect / atlas machinery. 128^2 RGBA8
    // is 64 KiB per tile, so the browser's 256-tile ceiling is 16 MB.
    constexpr uint32_t kThumbnailSize = 128;

    // Queue ceiling. The asset browser can mark hundreds of material tiles
    // wanted in one frame and the lane drains ONE per frame, so an unbounded
    // deque would grow a copy of a MaterialComponent (a large struct) per
    // visible tile and hold it for minutes. Past the cap the enqueue is refused
    // and the entry simply stays un-tiled until a later frame asks again -
    // which is the same retry the eviction sweep already produces.
    constexpr uint32_t kMaxThumbnailQueue = 256;

    // STORAGE cap: how many live view ids can exist at once; not a free parameter.
    // Each live view owns one device HDR slot, and so does the thumbnail lane.
    // GraphicsDX12::kFirstViewHdrSlot is 6 (the slots from it on are the renderer's
    // secondary views'):
    //     slot 0        the main viewport, for the life of the process
    //     slots 1..N    live preview panels (this constant)
    //     one more      the thumbnail lane's own target
    // so the live supply is kFirstViewHdrSlot - 2. A larger number would hand out view ids
    // PrepareFrame can never build a target for (AcquireView says yes and the panel
    // sits on "rendering..." forever). PreviewRenderer.cpp, the one TU that sees both,
    // has static_assert(kMaxPreviews + 1 <= GraphicsDX12::kFirstViewHdrSlot - 1), so
    // raising this alone fails the build.
    constexpr uint32_t kMaxPreviews = 4;

    // SCHEDULING cap: how many views may be rendered in one frame. Separate from the
    // storage cap and enforced with ENGINE_CHECK_ONCE, because the failure it guards
    // (two dispatches sharing one FrameCB slot) has no crash and no debug-layer
    // message, only intermittent flicker.
    //
    // 2 is a budget, not a capability: one lane-0 picture (a Synthetic view or a
    // thumbnail) plus one WorldCull picture, so a thumbnail is not starved by a camera
    // view, which is dirty every frame. Split viewports share the one m_world GraphSet
    // and therefore one lane, so they take turns (ProcessQueue orders candidates by
    // Target::lastTickFrame and prefers the hovered cell): four views at
    // tickIntervalFrames 1 each refresh at about a quarter of the frame rate, for the
    // cost of one camera view. Each lane is its own set of constant-buffer rings with
    // one FrameCB slot per frame in flight, so raising this needs another lane, not
    // just permission; RecordRequest's per-lane ENGINE_CHECK trips otherwise.
    constexpr uint32_t kMaxPreviewsPerFrame = 2;

    // ---- Clustered-light record ----
    // Used by the studio rig's point lights (lane 0) and a WorldCull view's scene
    // lights (lane 1). The one GPULight C++ and every shader share
    // (shaders/shared/gpu_light.hlsli); the grid shape (kClusterCount) comes from
    // ClusterPass.h. Named in this seam header because the GAME half packs it:
    // Renderer::ExtractPreviewSceneLights culls the scene's punctual lights per
    // WorldCull view into PreviewRenderer's fixed-size per-view cache, and the render
    // half memcpys that cache into its UPLOAD ring.
    using GPULight = GpuShared::GPULight;

    // LightGridEntry from cluster_common.hlsli.
    struct LightGridEntry { uint32_t offset; uint32_t count; };
    static_assert(sizeof(LightGridEntry) == 8,
                  "Preview::LightGridEntry must match LightGridEntry in cluster_common.hlsli");

    // ---- The one light cap: it sizes the buffers ----
    // How many GPULight records a preview lane's UPLOAD ring holds, for both lanes (the
    // studio rig on lane 0 uses 2; lane 1 carries a secondary view's scene lights).
    // The allocation in EnsureClusterBuffers, the memcpy bound in both writers, the
    // game half's per-view staging array and the ENGINE_CHECK before every write all
    // read this. Do not split it into two caps: a count from one size memcpy'd into
    // the other walks off a mapped UPLOAD buffer with no debug-layer message.
    //
    // 16 comes from the cost ladder of preview_pip_lights_cost (in-process ON/OFF
    // renderPhaseMs / renderThreadMs delta). Per pixel it costs n 64-byte loads from
    // an uncached UPLOAD StructuredBuffer plus a range test each, through Lighting.ps's
    // WaveActiveAllEqual scalar path (which the uniform grid nearly always takes).
    // Raising this means re-running that ladder: it is the only lever on the feature's
    // GPU cost (see the uniform-grid argument on PreviewRenderer's cluster members).
    constexpr uint32_t kMaxPreviewLights = 16;

    // Idle-teardown grace, in ServiceReleases frames (one per engine frame). A
    // GraphSet whose views have all been gone this long is destroyed (physical
    // textures, pass instances and everything the passes created) and rebuilt lazily
    // on the next request. A grace rather than immediate teardown, for three reasons:
    //   1. Quiescence. Teardown frees descriptors and DescriptorAllocation::Free is
    //      immediate; waiting out at least kFramesInFlight frames with no records is
    //      what makes that safe without a hard sync.
    //   2. The tone-map output. The same sweep releases each Target's ToneMapPass,
    //      whose output an earlier ImGui::Image captured a descriptor of; ImGui's draw
    //      data outlives a panel close by a frame. Same floor.
    //   3. Hysteresis, which is why it is 300 rather than 4. The thumbnail queue goes
    //      empty between tiles and a panel closes and reopens on a tab switch, so
    //      "idle" happens dozens of times a minute, and a rebuild (seven CreateTexture
    //      calls plus five pass Init()s) costs far more than the ~11 MB at 512^2 it
    //      reclaims.
    // 300 frames is 5 s at 60 Hz: longer than any browser-burst or tab-switch gap,
    // short enough that a closed panel does not hold VRAM for a session. Only the
    // harness may shorten it (-preview_idle_frames=<N>).
    constexpr uint32_t kIdleTeardownFrames = 300;
    // The floor the override is clamped to, and the reason is (1) and (2)
    // above: fewer than this many frames of quiet cannot prove the GPU is done
    // with the descriptors being handed back.
    constexpr uint32_t kMinIdleTeardownFrames = RHI::kFramesInFlight + 1;

    // The ViewBit a secondary (WorldCull) preview view culls against: bit 6,
    // ViewBit::SecondaryView, which nothing else sets or tests. Claimed here rather
    // than a private filter, so an author who wants a helper hidden from the PiP clears
    // the same mask as for any other view. The main path tests ViewBit::MainCamera and
    // the shadow path ShadowCascade*, never this bit, so clearing it means "not in the
    // secondary view", not "not rendered". The default VisibilityComponent::viewMask
    // is ViewBit::All, so by default the PiP sees what the main camera sees.
    constexpr uint32_t kSecondaryViewBit = ViewBit::SecondaryView;
    static_assert(kSecondaryViewBit != ViewBit::MainCamera,
                  "the secondary preview view must not share the main camera's bit");

    constexpr uint32_t kInvalidView = ~0u;

    // Which primitive carries the material. Mirrors PrimitiveMeshType's first
    // five entries minus Cone (a cone shows less of a material than any of
    // these and nobody asked for it); the mapping to PrimitiveMeshType lives in
    // PreviewRenderer.cpp, which is the one TU that sees both.
    enum class Shape : uint8_t { Sphere = 0, Cube = 1, Plane = 2, Torus = 3 };

    // ViewDesc feature bits.
    namespace Feature
    {
        // Draw a SkyboxPass#2 backdrop behind the subject instead of the flat clear
        // colour. ViewDesc::background picks the cube: Checker binds a preview-owned
        // procedural checker cube, Skybox the studio radiance cube (or the live scene's
        // sky, with kSceneLighting). The widget derives this bit from
        // ViewDesc::background; it exists separately because the preview view's Sky flag
        // is keyed on it (PreviewRenderer::ApplyPreviewView).
        constexpr uint32_t kSkybox = 1u << 0;

        // Compatibility bit, a no-op: it means "no constant ambient", which always holds
        // because fill and back are clustered point lights, so -preview_parity can keep
        // passing it. kAmbientFill is the inverse and the one that does something (it
        // restores the constant ambient stand-in); nothing sets it.
        constexpr uint32_t kNoAmbientFill = 1u << 1;
        constexpr uint32_t kAmbientFill   = 1u << 2;

        // Light the subject with the live scene's sky instead of the studio rig: the main
        // pipeline's current irradiance / radiance / BRDF handles as SyncSkyboxIBL
        // published them, its sun direction and colour, and its sky SH.
        // Opt-in because it makes the preview non-deterministic: a scene with time of day
        // hands it a different sun every frame, so two runs of a pack disagree and a cached
        // thumbnail goes stale. Thumbnails never set it (the thumbnail lane builds its own
        // ViewDesc), so the disk cache stays keyed on a picture that cannot drift.
        constexpr uint32_t kSceneLighting = 1u << 3;

        // Render the key light's 512^2 depth slice (PreviewStudioShadowPass) and hand
        // LightingPass#2 / TransparentPass#2 a one-cascade CSM. On by default for the live
        // panel; off for thumbnails (a 128^2 tile does not resolve a shadow) and for
        // -preview_parity unless the parity world is built with the matching CSM.
        constexpr uint32_t kStudioShadow = 1u << 4;

        // Bit 5 is free. Do not claim it for a flag nothing sets: forward vs deferred is
        // decided on the render half from req.mat.userBlendMode, and the scene-lights
        // switch is the process-wide Renderer::SetPreviewWorldSceneLights
        // (-preview_pip_lights=<0|1>, Lua Preview.SetWorldSceneLights). Add a per-view form
        // only when a caller needs one.

        // Drop the clustered fill + back lights: key light plus IBL only. Two uses: a
        // rollback lever for the clustered lights that leaves the buffers, the grid and
        // the LightCB write untouched, and the way the back light is measured - the same
        // binary renders the same fixture with and without the two point lights, so the
        // difference (e.g. transmission on sss / twosided) is exactly those lights.
        constexpr uint32_t kKeyLightOnly = 1u << 6;
    }

    // Backdrop selector. Solid is the default; Checker and Skybox both go through
    // SkyboxPass#2 and differ only in the cube sampled, so neither needs a fullscreen
    // pass, a new shader or a second optimised clear value.
    enum class Background : uint8_t
    {
        Solid   = 0,   // ViewDesc::clearColor, i.e. the graph's HDR clear
        Checker = 1,   // preview-owned procedural checker TextureCube
        Skybox  = 2,   // the studio radiance cube (or the scene sky)
    };

    // Where the draw list comes from:
    //   Synthetic - no World, entity, instance slot or material slot: hand-built
    //               packets against a private instance table and material table (up
    //               to kMaxPreviewParts parts, one material record each). The
    //               material inspector, Asset Preview, Mesh Editor and thumbnail lane.
    //   WorldCull - a second frustum cull of the live world on the render half
    //               (SceneBVH::FrustumCull, then a filter of the frame's Opaque and
    //               Transparent DrawLists), drawn against the scene's instance and
    //               material buffers. The camera-preview window and the in-game PiP.
    // The two cannot share a graph instance: a RenderGraph has one size and a
    // WorldCull view differs in size from an inspector panel, so PreviewRenderer keeps
    // a separate GraphSet for WorldCull views.
    enum class Source : uint8_t { Synthetic = 0, WorldCull = 1 };

    // What to draw and how big. Compared by value to decide whether a request
    // is a new picture or the same one again (see PreviewRenderer::SetRequest).
    struct ViewDesc
    {
        struct PartMesh
        {
            uint32_t placement    = 0;
            uint32_t meshDescSlot = 0xFFFFFFFFu;
            uint32_t indexCount   = 0;
            bool operator==(const PartMesh&) const = default;
        };

        uint32_t w = 0, h = 0;
        // Synthetic keeps every field below meaningful; WorldCull ignores shape / mesh
        // override / orbit / studio rig and takes its camera from Request::cam, which the
        // GAME half resolves from the target entity every tick.
        Source   source = Source::Synthetic;
        // WorldCull only: render at most one frame in N. 1 = every frame (a focused view,
        // the in-game PiP), higher for an unfocused panel. Ignored by Synthetic views,
        // which are dirty-edge gated.
        // N counts PREVIEW frames, not engine frames: the gate reads
        // PreviewRenderer::m_previewFrame, which ProcessQueue increments once per frame on
        // which the preview layer is serviced. With no panel open it does not advance, and
        // with several views live the scheduler draws one per preview frame, so a view refreshes
        // once every (views x tickIntervalFrames) preview frames, paused or running. That
        // makes it a refresh budget, not a metronome: a pack asserting the cadence reads
        // the counter (Lua `Preview.PreviewFrame()`, C++ `Renderer::GetPreviewFrame()`),
        // not `Engine.Frame()`.
        uint32_t tickIntervalFrames = 1;
        // Background. GBufferPass#2 declares BuiltinTexture::HdrSceneColor (bind +
        // CLEAR), so this is the graph's clear colour.
        // Alpha is 0, not 1: CreateHdrTarget gives the preview slot an optimised clear
        // value of {0,0,0,0} and D3D12 compares all four components, so {0,0,0,1} would
        // raise CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE on every rendered preview
        // frame. Nothing composites this alpha (the tone map writes its own). The editor's
        // present composition buffer is a separate clear site with the same warning
        // (SurfaceDX12::EnsurePresentComposition); debug-layer preview packs gate on the
        // warning text for both.
        float    clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
        uint32_t features = 0;

        Shape    shape = Shape::Sphere;
        Background background = Background::Solid;
        // Orbit camera, applied on top of PreviewStudioRig's defaults. Stored as deltas
        // so "the user never dragged" is bit-identical to the rig table, which the A/B
        // parity fixture needs.
        float    yawOffset   = 0.f;   // radians
        float    pitchOffset = 0.f;   // radians
        float    distanceScale = 1.f; // multiplies StudioCamera::distance

        // ---- Per-material mesh override ----
        // A .meshlib path plus the mesh id inside it; empty = the primitive named by
        // `shape` (the default everywhere, thumbnails included). A path, not a resolved
        // slot: the widget runs on the editor UI half, and resolving a mesh-descriptor
        // slot needs MeshLibrary + MeshManager, which belong to the GAME half.
        // ExtractRequests fills Request::meshDescSlot / meshIndexCount, as it does
        // matRuntimeKey and customPSID. A .imsh is not accepted: there is no runtime path
        // from a .imsh to a mesh-descriptor slot for any caller; .meshlib + meshId is what
        // the scene loader uses.
        std::string meshPath;
        uint32_t    meshId = 0;

        // ---- Asset source ----
        // An asset drawn as N packets. Empty / AssetKind::None is the single-primitive
        // path, the default for the material inspector and every material thumbnail.
        // Separate from meshPath on purpose: meshPath draws the inspector's material on
        // another mesh, while an asset brings its own materials, one per part, and the
        // request's single `mat` is meaningless. One field for both would make "which
        // material does this draw with" depend on which string happened to be set.
        AssetKind   assetKind = AssetKind::None;
        std::string assetPath;

        // ---- Mesh Editor ----
        // Every default below is the picture other callers always had, so a ViewDesc that
        // never sets them compares equal. Synthetic only; the WorldCull lane ignores them.
        // viewMode: a Renderer::ViewMode value (Lit 0, Unlit 1, Wireframe 2, BaseColor 12
        // .. Depth 17) written into this view's LightCB.viewMode; the main viewport's mode
        // is untouched. Wireframe also switches the preview GBuffer/Transparent passes to
        // FILL_MODE_WIREFRAME for this record, and every non-Lit mode hides the backdrop.
        uint32_t viewMode = 0;
        // A world-space shift of the eye AND of the framed point, in the
        // fitted unit box (Preview::StudioCameraBasis' panOffset).
        float    pan[3] = { 0.f, 0.f, 0.f };
        // Asset views: which composed PLACEMENTS are drawn, bit i = placement
        // i of the composition (for a MeshAsset: section i of the Mesh). The
        // framing fit is the WHOLE asset's either way, so isolating a section
        // does not move the camera. Placements past 63 are always drawn.
        uint64_t partMask = ~0ull;
        // Asset views: >= 0 draws every OTHER placement in a plain grey
        // material (the engine default, custom shaders dropped), so the
        // section picked in a list reads at a glance. -1 = none.
        int32_t  highlightPart = -1;
        // Asset views: the material placement i draws with INSTEAD of the one
        // the asset names (an empty string or a short vector = the asset's
        // own). The Mesh Editor's STAGED slot edits, shown before they are
        // saved; the composition itself - memoised per asset - is untouched.
        std::vector<std::string> partMaterials;

        // ---- Mesh Editor shape keys ----
        // Asset views: placement `placement` draws mesh-descriptor slot `meshDescSlot`
        // (`indexCount` indices) instead of the one its library entry resolved to - the
        // Mesh Editor's shape-key preview, whose slots it owns and re-points at morphed
        // copies of the section. The library's slot is shared with every placement of that
        // entry in the world and is never touched. Empty = the asset's own geometry.
        std::vector<PartMesh> partMeshes;
        // Bumped by the owner of those slots whenever it rewrites what they
        // point at: the slot numbers do not move, so without this a new shape
        // would never re-render.
        uint64_t partMeshRevision = 0;
        // Placements drawn in a white material that multiplies in the VERTEX
        // COLOUR (MaterialComponent::USE_VERTEXCOLORS, custom shaders dropped) -
        // the shape-key heatmap. Bit i = placement i; 0 = none.
        uint64_t vertexColorParts = 0;
    };

    inline bool operator==(const ViewDesc& a, const ViewDesc& b)
    {
        return a.w == b.w && a.h == b.h && a.features == b.features
            && a.source == b.source
            && a.tickIntervalFrames == b.tickIntervalFrames
            && a.shape == b.shape
            && a.background == b.background
            && a.yawOffset     == b.yawOffset
            && a.pitchOffset   == b.pitchOffset
            && a.distanceScale == b.distanceScale
            && a.clearColor[0] == b.clearColor[0]
            && a.clearColor[1] == b.clearColor[1]
            && a.clearColor[2] == b.clearColor[2]
            && a.clearColor[3] == b.clearColor[3]
            && a.meshId   == b.meshId
            && a.meshPath == b.meshPath
            && a.assetKind == b.assetKind
            && a.assetPath == b.assetPath
            && a.viewMode  == b.viewMode
            && a.pan[0] == b.pan[0] && a.pan[1] == b.pan[1] && a.pan[2] == b.pan[2]
            && a.partMask  == b.partMask
            && a.highlightPart == b.highlightPart
            && a.partMaterials == b.partMaterials
            && a.partMeshes == b.partMeshes
            && a.partMeshRevision == b.partMeshRevision
            && a.vertexColorParts == b.vertexColorParts;
    }
    inline bool operator!=(const ViewDesc& a, const ViewDesc& b) { return !(a == b); }

    // GAME -> RENDER. One latch, not a double buffer: hanging it off
    // m_sceneWrite/m_sceneRead would break that pair's "one pair for the whole scene
    // payload" contract, and this lane is driven by UI events, not the scene rebuild.
    struct Request
    {
        bool     valid     = false;
        // Set by the extract when the request is handed over; cleared by the record - on
        // success and on an early-out alike (a request that did not fly must not look like
        // it did) - and defensively again by the apply. SetRequest must not clear it: a
        // request landing between extract and record would let the next extract hand the
        // lane over again while the first is still in flight, and the record would publish
        // the new generation for a picture nobody drew (an ABA hole).
        bool     inFlight  = false;
        uint32_t viewId    = kInvalidView;
        // ABA guard: a result whose generation no longer matches the view's is dropped, so
        // a superseded (colour dragged) or invalidated (panel closed, view id recycled)
        // request cannot stamp "done" onto the wrong record.
        uint32_t generation = 0;
        ViewDesc view;

        // ---- WorldCull payload ----
        // Resolved on the GAME half like matRuntimeKey and customPSID: it comes from the
        // target entity's GlobalTransform + CameraComponent (or VirtualCameraComponent),
        // and the record half runs under ThreadCtx::Render and may not read World.
        // Renderer::ExtractPreviewCameras resolves it; the editor window and the
        // Lua/harness PiP only name an entity. Left at its default for a Synthetic view,
        // whose camera is the studio rig.
        RenderCamera cam{};
        // True when the resolver found a live camera entity this frame. False
        // makes the record bail rather than draw the default RenderCamera at
        // the world origin, which looks like a real picture of nothing.
        bool     camValid = false;
        // The ViewBit this view culls against (kSecondaryViewBit for every WorldCull
        // view). Carried in the request so a per-view bit would be a data change.
        uint32_t viewBit  = kSecondaryViewBit;

        // ---- Material payload ----
        // A full value copy of the MaterialComponent being edited, taken on the half that
        // owns the editor buffer. The record half runs under ThreadCtx::Render and must not
        // reach into World or editor state (violations report as [SPLITCHECK][RENDERCTX]);
        // the copy lets it run the same Renderer::WriteMaterialSlot as the main path, so
        // the record is byte-identical by construction.
        MaterialComponent mat;
        // BuildMaterialRuntimeKey(mat): names the shared RuntimeMaterialResource that owns
        // this material's texture handles and bindless indices. The record half
        // re-resolves it by key every frame and never caches the pointer: the last world
        // entity using the same material can drop its refcount at any frame boundary.
        uint64_t matRuntimeKey = 0;
        // Renderer::HashMaterialParams(mat, rt): the only correct dirty key for "the
        // picture would differ" - SetDirty is consumed and cleared every frame, and
        // IsAssetDirty is a level, not an edge.
        uint64_t matHash = 0;
        // 0 = the engine GBuffer PS/VS. Resolved on the GAME half only:
        // ShaderLibrary::RegisterDynamic reallocates a vector another thread may hold a raw
        // entry pointer into.
        uint32_t customPSID = 0;
        uint32_t customVSID = 0;
        // False while any texture the material references is still streaming.
        // The draw still happens (a white sphere now beats a grey box later),
        // and the view re-renders when this flips - see PreviewRenderer.
        bool     texturesReady = false;

        // ---- Asset payload, resolved ----
        // Non-empty only for an asset view (assetKind != None), and then it replaces `mat`
        // / `matRuntimeKey` / `customPSID` / `meshDescSlot`: BuildDrawList emits one packet
        // per entry. An empty vector on an asset view means "not streamed yet" or "does not
        // parse", and the record bails rather than fall back to the primitive sphere,
        // which would read as "the prefab looks like this".
        std::vector<AssetPart> parts;
        // How many DISTINCT runtime-material keys `parts` names. Reported by the
        // panel and by the [PREVIEW] asset: log line, because "12 packets" and
        // "12 materials" are different claims and only the second one says the
        // by-key material runtime resolved each part separately.
        uint32_t partMaterialCount = 0;
        // How many parts the resolver DROPPED at kMaxPreviewParts. Non-zero
        // means the picture is deliberately partial and the panel says so.
        uint32_t partsDropped = 0;
        // Node kinds the prefab walk declined - the other reason a picture is partial - so
        // the panel can say how much was omitted, and only for prefabs that omit something.
        // Copied from the memoised AssetComposition (one struct copy per resolve).
        Resource::PrefabSkipCounts skipped{};

        // ---- Mesh override, resolved ----
        // ExtractRequests resolves ViewDesc::meshPath / meshId on the GAME half
        // (MeshLibrary::Load and MeshManager::RegisterMeshLibMesh touch descriptor-heap
        // state). kInvalidBufferIndex or a zero index count means "use the primitive",
        // which is also what a failed load produces.
        uint32_t meshDescSlot   = 0xFFFFFFFFu;
        uint32_t meshIndexCount = 0;
        // Uniform scale + centre offset mapping the override mesh's AABB into the unit box
        // the primitives occupy, so the studio camera frames it without zooming. Identity
        // (1, 0,0,0) for the primitive path, which keeps that path bit-identical.
        float    meshFitScale   = 1.f;
        float    meshFitOffset[3] = { 0.f, 0.f, 0.f };
    };

    // RENDER -> GAME.
    struct Result
    {
        uint32_t viewId     = kInvalidView;
        uint32_t generation = 0;
        bool     ok         = false;
    };

    // ==== One-shot (thumbnail) lifecycle ====
    // A different state machine from Request, not a reuse of it:
    //   * `valid` is set by the browser's dirty sweep when it finds an asset with no
    //     tile, not every frame;
    //   * `generation` is bumped per invalidation (a chain save, a texture re-import,
    //     a shader edit), not per extract, so a thumbnail invalidated twice while one
    //     render was in flight drops the first result on arrival;
    //   * the dirty flag lives on the editor's AssetEntry and is cleared only at
    //     apply. A record early-out clears inFlight and leaves dirty set; otherwise the
    //     asset would drop out of the queue for the session.
    // The lane has its own queue, latch, target and graph, so the asset browser never
    // competes for a live view's latch. It shares lane 0's constant-buffer rings with
    // the live Synthetic views: ProcessQueue records at most one of {live Synthetic
    // view, thumbnail} per frame, and a dirty live view wins.
    struct ThumbRequest
    {
        // Stable identity of the asset this tile belongs to. The editor uses
        // the low 64 bits of the asset's guid (AssetGuidIndex), or a hash of
        // its canonical path when it has no sidecar - either way it is stable
        // across a rescan, which an index into m_assets is not.
        uint64_t assetKey = 0;
        // Bumped by the EDITOR per invalidation. A result whose generation no
        // longer matches is dropped rather than marking the tile ready.
        uint32_t generation = 0;
        // The disk state of the material (Resource::LoadMaterial), by value - the record
        // half runs under ThreadCtx::Render and may not reach into World or editor state.
        // The tile always shows what is on disk; unsaved inspector edits are shown with a
        // dot, not baked.
        // Meaningless for an asset tile (assetKind != None), which draws the asset's own
        // materials, one per part; the editor passes a default component.
        MaterialComponent mat;
        // An asset tile instead of a material one. ExtractThumbnails resolves the parts on
        // the GAME half from this path, as for the live lane; the editor names an asset,
        // it never resolves one.
        AssetKind   assetKind = AssetKind::None;
        std::string assetPath;
        // Filled in by ExtractRequests on the GAME half, exactly as the live
        // lane's are: none of these can be resolved from the record half.
        uint64_t matRuntimeKey = 0;
        uint32_t customPSID = 0;
        uint32_t customVSID = 0;
        bool     texturesReady = false;
        // How many times this request has been put back for "textures are still
        // streaming". A thumbnail is BAKED TO DISK, so unlike the live panel it
        // must not draw the white fallback - see the note in ExtractRequests.
        uint16_t retries = 0;
        // The destination, created by the EDITOR in its serial window and owned
        // by the AssetEntry. SRV-only, R8G8B8A8_UNORM, kThumbnailSize square,
        // and by contract it is in SHADER_RESOURCE on entry and on exit.
        RHI::Texture tile;
    };

    struct ThumbResult
    {
        uint64_t assetKey   = 0;
        uint32_t generation = 0;
        bool     ok         = false;
    };
}
