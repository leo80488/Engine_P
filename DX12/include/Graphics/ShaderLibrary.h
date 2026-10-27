#pragma once

// ShaderLibrary: compiled shader bytecode and GPU shader handles for one pass or subsystem.
// Per (ShaderID or dynamic id, PermutationKey):
//   1. derive the .ishdr cache path (ShaderDiskCache::CachePath);
//   2. a cache file at least as new as the source closure: read its DXIL, no recompile;
//   3. otherwise compile the .hlsl with DXC (SM 6.6) and the permutation #defines, and
//      write the .ishdr;
//   4. IGraphicsDevice::CreateShader -> RHI::Shader handle, cached in memory.
// Per frame, GetShader() is a hash-map lookup: no compilation.
// No DX12 types (ID3D12*, DXGI_FORMAT) appear in this header.

#include "Graphics/DynamicShaderRegistry.h"   // ShaderSourceEntry
#include "Graphics/GraphicsStruct.h"
#include "Graphics/PermutationKey.h"
#include "Graphics/ShaderReflection.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

// GetDynamicDecl's type (Graphics/MaterialShaderDecl.h).
namespace MaterialShaderDecl { struct Decl; }

class IGraphicsDevice;

// Worker-side shader and pipeline warm-up (the prefab preload) holds a shared scope while
// it uses a library's cache entries (a ShaderEntry, a reflection, a merged union) or builds a
// pipeline from them with the library's mutex dropped; ShaderLibrary::ClearCaches and
// PSOCache::Clear take an exclusive scope first. So a hot reload waits for the warm-up's
// current material instead of freeing a node under it, and no warm-up build straddles a
// clear (a variant compiled from the old source, cached after the reload "succeeded").
// Main and the record lanes never take it shared: a reload drains the lanes first, and main
// is the clearing thread. Both scopes are re-entrant on their own thread.
class ShaderCacheGate
{
public:
    class Shared
    {
    public:
        Shared();
        ~Shared();
        Shared(const Shared&)            = delete;
        Shared& operator=(const Shared&) = delete;
    private:
        bool m_locked = false;
    };
    class Exclusive
    {
    public:
        Exclusive();
        ~Exclusive();
        Exclusive(const Exclusive&)            = delete;
        Exclusive& operator=(const Exclusive&) = delete;
    private:
        bool m_locked = false;
    };
};

// Identifies a named shader within the engine's library.
// Maps to a specific .hlsl file + stage + entry point.
enum class ShaderID : uint32_t
{
    GBuffer_VS          = 0,
    GBuffer_PS          = 1,
    Lighting_VS         = 2,
    Lighting_PS         = 3,
    PickingID_VS        = 4,
    PickingID_PS        = 5,
    Skybox_VS           = 6,
    Skybox_PS           = 7,
    BloomDownsample_CS  = 8,
    BloomUpsample_CS    = 9,
    HistogramBuild_CS   = 10,
    HistogramAverage_CS = 11,
    ToneMap_CS          = 12,
    Transparent_PS      = 13,
    Shadow_VS              = 14,
    Shadow_PS              = 15,
    TAA_CS                 = 16,
    OutlineHull_VS         = 17,
    OutlineHull_PS         = 18,
    OutlineObjectID_PS     = 19,
    OutlineScreenSpace_PS  = 20,
    SkinCS                 = 21,  // Compute skinning: restPose + blendData + poseMatrices -> skinnedVertices
    ClusterBuild_CS        = 25,  // Clustered lighting: build cluster AABBs
    ClusterCull_CS         = 26,  // Clustered lighting: assign lights to clusters
    GenerateLUT_CS         = 29,  // Color grading: bake procedural 3D LUT
    InstanceCull_CS        = 30,  // GPU frustum culling compute shader
    HiZGenerate_CS         = 31,  // Hi-Z mip chain generation
    DebugWire_VS           = 32,  // Debug wireframe vertex shader
    DebugWire_PS           = 33,  // Debug wireframe pixel shader
    XeGTAO_CS              = 34,  // XeGTAO main AO compute shader
    XeGTAODenoise_CS       = 35,  // XeGTAO spatial denoise compute shader
    SkySHProjection_CS     = 36,  // Sky -> L2 SH projection (9 float3 diffuse IBL)
    SpecularPrefilter_CS   = 37,  // GGX split-sum specular cubemap pre-filter (one mip per dispatch)
    SkyAtmosphere_CS       = 38,  // Hillaire atmospheric cubemap generator (samples SkyView LUT)
    AtmosphereTransmittance_CS = 39,  // Hillaire Transmittance LUT (256x64, once)
    AtmosphereMultiScatter_CS  = 40,  // Hillaire MultiScatter LUT (32x32, once)
    AtmosphereSkyView_CS       = 41,  // Hillaire SkyView LUT (192x108, per-frame)
    AerialPerspective_CS       = 42,  // Hillaire Aerial Perspective 3D LUT (32x32x32, per-frame)
    FroxelDensity_CS           = 43,  // Volumetric fog: per-froxel medium density
    FroxelLightInject_CS       = 44,  // Volumetric fog: per-froxel light injection
    FroxelScatter_CS           = 45,  // Volumetric fog: front-to-back integration
    VolumetricApply_VS         = 46,  // Volumetric fog: composite fullscreen tri (VS)
    VolumetricApply_PS         = 47,  // Volumetric fog: composite (PS)
    FroxelTemporal_CS          = 48,  // Volumetric fog: temporal reprojection ping-pong
    SceneVoxelize_CS           = 49,  // Scene occupancy voxelization for volumetric-light occlusion
    SceneVoxelClear_CS         = 50,  // Zeroes the occupancy grid at the start of each voxelize frame
    VolumetricRaymarch_CS      = 51,  // Per-pixel (half-res) raymarch for sun + shadow-casting spots
    VolumetricRaymarchTemporal_CS = 52,  // Temporal reprojection of the half-res raymarch output
    VolumetricRaymarchApply_PS = 53,  // Composite PS: half-res raymarch -> HDR (additive)
    XeGTAODepthLinearize_CS    = 54,  // XeGTAO: hardware depth -> linearized
    StarsBake_CS               = 55,  // One-time bake of the night-sky star cubemap
    ParticleEmit_CS            = 56,  // Particle system: emit new particles into pool
    ParticleUpdate_CS          = 57,  // Particle system: advance all alive particles
    Particle_VS                = 58,  // Particle system: billboard VS
    Particle_PS                = 59,  // Particle system: radial-falloff sprite PS
    TrailUpdate_CS             = 60,  // Trail system: append + age segments
    Trail_VS                   = 61,  // Trail system: ribbon extrusion VS
    Trail_PS                   = 62,  // Trail system: flat color + age alpha
    XeGTAOTemporal_CS          = 63,  // XeGTAO: AO-only temporal accumulation pass
    CAS_CS                     = 64,  // AMD FidelityFX Contrast Adaptive Sharpening
    ProbeCapture_PS            = 65,  // Reflection-probe baker forward PS (sun + SH ambient -> 1 RTV)
    ProbeCaptureSky_PS         = 66,  // Reflection-probe baker sky PS (sample one cubemap, no sun disk)
    ClusterCullProbes_CS       = 67,  // Cluster culling for reflection probes (mirrors ClusterCull layout)
    HiZReduce_CS               = 68,  // Hi-Z mip N = reduce(mip N-1), UAV -> UAV, no per-subresource transitions
    SSRTrace_CS                = 69,  // Screen-space reflection trace (stochastic GGX rays, Hi-Z traversal)
    SSRComposite_CS            = 70,  // SSR composite - blend hit-UV reflection into HDR
    SSRDepthHierarchyMip0_CS   = 71,  // SSR depth hierarchy mip 0 (depth SRV -> R16G16 max/min)
    SSRDepthHierarchyReduce_CS = 72,  // SSR depth hierarchy mip N = reduce(mip N-1), UAV -> UAV
    SSRResolve_CS              = 73,  // SSR spatial reuse + variance + reprojection-depth
    SceneColorPyramidMip0_CS   = 74,  // HDR pre-filtered pyramid mip 0 CS (SceneColorPyramidPass copies mip 0 instead)
    SceneColorPyramidReduce_CS = 75,  // HDR pre-filtered pyramid reduce (Karis firefly-weighted)
    SSRTemporal_CS             = 76,  // SSR temporal reprojection + YCoCg variance clip
    SSRUpsample_CS             = 77,  // SSR variance-driven bilateral blur (final stage)
    DecalClusterCull_CS        = 78,  // Clustered decals: assign decal OBBs to clusters
    DecalApply_CS              = 79,  // Clustered decals: per-pixel blend into GBuffer via UAV
    GlassShatterInit_CS        = 80,  // Glass shatter: per-shard physics initial state
    GlassShatterSimulate_CS    = 81,  // Glass shatter: per-frame physics integration
    GlassShatterComposite_CS   = 82,  // Glass shatter: per-pixel polygon test + frozen-source sample
    TracerEmit_CS              = 83,  // Tracer system: copy CPU spawn queue into GPU pool
    TracerUpdate_CS            = 84,  // Tracer system: age all alive tracers
    Tracer_VS                  = 85,  // Tracer system: cylindrical-billboard quad VS
    Tracer_PS                  = 86,  // Tracer system: scrolling-noise + soft-fade PS
    BeamTubeGen_CS             = 87,  // Heavy-beam: Parallel-transport tube vertex generation
    Terrain_MS                 = 88,  // Mesh-shader terrain: per-tile vertex + index emission
    Terrain_PS                 = 89,  // Mesh-shader terrain: GBuffer output (PBR layers, height-blended)
    Terrain_Shadow_MS          = 90,  // Mesh-shader terrain: depth-only for CSM cascades
    Terrain_AS                 = 91,  // Amplification shader: per-sub-tile frustum cull (colour pass)
    Terrain_Shadow_AS          = 92,  // Amplification shader: per-cascade frustum cull (shadow pass)
    UI_VS                      = 93,  // UI: orthographic quad VS, ByteAddressBuffer vertex fetch
    UI_PS                      = 94,  // UI: textured + tint PS
    WorldUI_VS                 = 95,  // World-space UI billboard VS (3D quads, viewProj)
    WorldUI_PS                 = 96,  // World-space UI textured + tint PS
    LensFlare_CS               = 97,  // Procedural directional-light lens flare (additive HDR)
    FXAA_CS                    = 98,  // NVIDIA FXAA 3.11 quality preset, single-pass HDR-aware
    AfterimageCopy_CS          = 100, // Afterimage: copy skinned pos/nrm slice into snapshot pool
    CloudNoiseBake_CS          = 101, // Volumetric clouds: bake 128^3 Perlin-Worley base shape (RGBA)
    CloudRaymarch_CS           = 102, // Volumetric clouds: quarter-res raymarch through cloud shell
    CloudComposite_VS          = 103, // Volumetric clouds: fullscreen-triangle VS
    CloudComposite_PS          = 104, // Volumetric clouds: bilinear upsample + alpha-over composite
    VideoComposite_VS          = 105, // Video playback: fullscreen-triangle VS
    VideoComposite_PS          = 106, // Video playback: NV12 Y/UV plane -> linear RGB composite
    VideoQuad_VS               = 107, // Video playback: world-space quad VS (procedural 6-vert)
    VideoQuad_PS               = 108, // Video playback: NV12 sample for world-space video quad
    Underwater_CS              = 109, // Underwater screen distortion + tint (HDR, pre-tonemap)
    DepthOfField_CS            = 110, // Focus-distance depth of field (HDR, depth-driven CoC gather)
    Stylize_CS                 = 111, // NPR stylize: Kuwahara/Posterize/Halftone/Dither/Crosshatch
    BillboardFX_VS             = 112, // Animated sprite-sheet billboard VS (3D quad, viewProj)
    BillboardFX_PS             = 113, // Animated sprite-sheet billboard PS (bindless atlas x HDR tint)
    Grass_AS                   = 114, // Procedural grass: per-patch frustum/distance cull + LOD
    Grass_MS                   = 115, // Procedural grass: Bezier blade generation (GoT-style)
    Grass_PS                   = 116, // Procedural grass: GBuffer write (deferred)
    Water_VS                   = 117, // Water surface: SV_VertexID grid over the water tile
    Water_PS                   = 118, // Water surface: Fresnel sky reflection + flow normals
    CloudDetailNoiseBake_CS    = 119, // Volumetric clouds: bake 32^3 high-freq Worley erosion noise
    CloudWeatherBake_CS        = 120, // Volumetric clouds: bake 512^2 weather map (coverage/type)
    HeightFogApply_PS          = 121, // UE-style analytic exponential height fog (fullscreen, post-cloud)
    ParticleBoidSnapshot_CS    = 122, // Boids: copy stable {pos,vel,valid} snapshot of the pool
    ParticleBoidSteer_CS       = 123, // Boids: Reynolds separation/alignment/cohesion steering (grid-accelerated)
    ParticleBoidGridClear_CS   = 124, // Boids: zero the spatial-grid per-cell counter
    ParticleBoidGridBuild_CS   = 125, // Boids: bin agents into the spatial grid (atomic scatter)
    WaterRippleStep_CS         = 126, // Interactive water: 2D wave-equation step + impulse inject
    FringeShadowMark_VS        = 127, // Bangs -> face screen-space shadow mark: view-offset proxy VS
    FringeShadowMark_PS        = 128, // Bangs -> face screen-space shadow mark: RT4.g flag write
    FaceOverlay_VS             = 129, // Brows/lashes-through-hair overlay: PVF pos+normal+uv VS
    FaceOverlay_PS             = 130, // Brows/lashes-through-hair overlay: hair-gated alpha redraw
    EditorGrid_PS              = 131, // Editor-only infinite XZ reference grid (fullscreen, pre-cloud)
    DebugWireDepth_PS          = 132, // Debug wireframe PS with manual (post-tonemap) occlusion fade
    // Inverted-hull outline drawn as real geometry into the 6-MRT GBuffer (OutlineGeometryPass),
    // so the rim writes depth + velocity and needs no TAA special-casing (OutlineHull_* is
    // OutlinePass's colour-only composite variant).
    OutlineGeometry_VS         = 133,
    OutlineGeometry_PS         = 134,
    NPRStabilize_CS            = 135, // AA-prepass NPR exposed-HDR shoulder
    // Water material domain: the engine default look for a Water-domain material without a
    // custom shader; a material with one replaces it via dp.customPSID, like GBuffer /
    // Transparent. Its own id rather than Transparent_PS on purpose: the surface permutation
    // mask applies per id, and a Transparent_PS entry would mask WATER_SURFACE into the key
    // space of the 1-RT forward main, producing a 1-render-target shader bound to a 4-RTV PSO.
    WaterSurface_PS            = 136,
    // ---- Material surface cache ----
    // UV-space bake VS: the same programmable-vertex-fetch chain as GBuffer_VS, but
    // SV_Position comes from uv0, so a mesh section rasterises into its own UV space and the
    // material's own pixel shader fills an atlas page.
    SurfaceCacheBake_VS        = 137,
    // Two ENTRY POINTS of the one SurfaceCacheResolve.cs.hlsl: `main` repacks
    // the bake scratch MRTs into the atlas page (GBuffer RT2 is
    // (rough, metal, ao, refl) while the RT proxy consumes (AO, rough, metal)),
    // `CSDilate` fills the unrasterised UV gutter so bilinear taps at a seam do
    // not fetch black.
    SurfaceCacheResolve_CS     = 138,
    SurfaceCacheDilate_CS      = 139,
    CloudTemporal_CS           = 140, // Volumetric clouds: reproject + accumulate the quarter-res raymarch
    CloudShadowBake_CS         = 141, // Volumetric clouds: bake the sun-direction shadow map cast on the world
    // Writes camera-only motion vectors into SKY pixels (reverse-Z depth == 0),
    // which no geometry pass covers. See VelocityFillPass.
    VelocityFill_CS            = 142,
    // Paints one render target over the main view's final output: the
    // viewport's Buffer menu / Render.target (DebugTargetViewPass).
    DebugTargetView_CS         = 143,
    // Paints one Texture Viewer picture into a panel-owned target (TextureViewPass).
    TextureView_CS             = 144,
    // One texel of a Texture Viewer picture read back (TextureViewPass probes).
    TextureViewProbe_CS        = 145,
    // The scene's mesh-shader GBuffer path.
    SceneMeshlet_AS            = 146, // per-cluster LOD selection + culling, 32 clusters a group
    SceneMeshlet_MS            = 147, // GBuffer.vs's vertex function, once per cluster vertex
    MeshletDebug_PS            = 148, // the Meshlets view mode's GBuffer PS
    // The CSM meshlet path (depth only; Shadow.ps for alpha test / dissolve).
    ShadowMeshlet_AS           = 149, // per-cluster LOD + frustum against one cascade
    ShadowMeshlet_MS           = 150, // Shadow.vs's vertex function, once per cluster vertex
    // The reactive mask FSR 2 / DLSS SR read, from the stencil bits TAA weakens history on
    // (UpscalerReactiveMaskPass).
    UpscalerReactiveMask_CS    = 151,
    // The debug targets no pass writes, made by the debug target view on the frames that show
    // or capture them (DebugTargetProduce.cs.hlsl).
    DebugTargetShadingModel_CS = 152,
    DebugTargetLightComplexity_CS = 153,
    DebugTargetOverdraw_CS     = 154,
    DebugTargetDensity_CS      = 155,
    DebugTargetMaterialCost_CS = 156,
    DebugTargetShadingCost_CS  = 157,
    // The shadow VS of a masked custom material that has no custom VS: custom_vertex.hlsli with
    // an empty hook, compiled with CUSTOM_VS_SHADOW + SHADOW_HOOK, so the material's SHADOW_HOOK
    // PS gets the full MaterialSurfaceVaryings in the shadow view.
    ShadowHook_VS              = 158,
    // The background behind the Transmissive surfaces, refracted and tinted, written into the
    // HDR scene before their colour pass (TransmissionApplyPass).
    TransmissionApply_PS       = 159,
    // The motion of what the Transmissive surfaces show, then its copy into the velocity buffer
    // (TransmissionMotionPass).
    TransmissionMotion_CS      = 160,
    TransmissionMotionWrite_CS = 161,
    // The reference path tracer's camera fog over the scene (PathTraceFogPass).
    PathTraceFogApply_PS       = 162,
    Count,
};

class ShaderLibrary
{
public:
    static constexpr uint32_t kInvalidDynShaderID = ~0u;

    // CompileForCache's result (an off-thread compile for the shader cache).
    struct CacheCompile
    {
        bool                 ok = false;
        std::string          cachePath;
        std::vector<uint8_t> container;
        std::string          error;
        double               ms = 0.0;
    };

    struct DynamicReads
    {
        bool                            known = false;
        std::unordered_set<std::string> vars;
        std::unordered_set<std::string> bindings;
    };

    struct ShaderReport
    {
        std::string file, entry, cachePath;
        uint32_t    permBits = 0;
        uint32_t    instructions = 0;
    };

    struct DynamicShaderStatus
    {
        uint32_t    compiled = 0;
        uint32_t    failed   = 0;
        uint32_t    failedPermBits = 0;   // the first failure's permutation
        std::string error;                // ... and its DXC output
    };

    // @p shaderDir: the .hlsl source directory, e.g. "shaders/" (a missing trailing separator
    // is added).
    void Init(IGraphicsDevice& gfx, const char* shaderDir);
    void Shutdown(IGraphicsDevice& gfx);

    // Registers a shader slot; call before GetShader or PreloadShader. @p hlslFile: the file
    // name inside shaderDir (e.g. "GBuffer.vs.hlsl"); @p entry: the HLSL entry point.
    void Register(ShaderID id, RHI::ShaderStage stage,
                  const char* hlslFile, const char* entry = "main");

    // Registers a runtime-discovered shader (e.g. a material's custom PS) and returns a dynamic
    // id that GetDynamicShader / GetDynamicReflection accept in every library, not just this
    // one: the id and its entry live in the process's one DynamicShaderRegistry, and the id is a
    // hash of (hlslFile, entry, stage), not a position. Re-registering the same triple returns
    // the same id, so it is safe once per frame from material setup code.
    // @p hlslFile: a bare file name lives under shaderDir (as for Register); a path with a
    // directory ("asset/Shaders/x.ps.hlsl") or an absolute one is used as is. Returns
    // kInvalidDynShaderID on failure.
    uint32_t RegisterDynamic(const char* hlslFile, RHI::ShaderStage stage,
                             const char* entry = "main");

    // READ-ONLY twin of RegisterDynamic: the id a previous RegisterDynamic (on
    // any library) gave the same (hlslFile, stage, entry) triple, or
    // kInvalidDynShaderID when nothing has registered it. It never inserts and
    // never compiles - for a caller that only wants to LOOK an id up (an editor
    // panel, an offline reflection walk).
    uint32_t FindDynamic(const char* hlslFile, RHI::ShaderStage stage,
                         const char* entry = "main") const;

    // Reflection reload epoch, bumped whenever the reflection cache is invalidated (ClearCaches
    // on the hot-reload path). Lets per-frame callers reduce RegisterDynamic +
    // GetDynamicReflection + MaterialReflectionSync to two integer compares: dynamic ids are
    // stable for the registry's lifetime, and a cached Reflection* stays valid until this number
    // changes (unordered_map element addresses are stable short of erasure). Starts at 1 so a
    // zero-initialised caller-side stamp reads as stale.
    uint32_t GetReloadEpoch() const { return m_reloadEpoch.load(std::memory_order_relaxed); }

    // The shader root this library compiles against ("shaders/"): where a bare file name lives
    // and what a material's own files are outside of. A capability a material declares
    // (#define MATERIAL_HAS_VERTEX_OFFSET) is read with it from the source before anything
    // compiles: MaterialShaderDecl::Get(path, GetShaderDir()).
    const std::string& GetShaderDir() const { return m_shaderDir; }

    // Compiles @p source as the text of @p hlslFile for one (@p stage, @p perm) with exactly the
    // options LoadOrCompile would use and returns the .ishdr it would write - registering
    // nothing, writing nothing. The caller writes the source file first and the container
    // after it, so the cache is not older than its source and the lazy load that follows is a
    // cache hit instead of a synchronous DXC run on the game half. Thread-safe: it reads only
    // what Init set. error: DXC's diagnostics (warnings too, when ok).
    CacheCompile CompileForCache(const std::string& hlslFile, const std::string& source, RHI::ShaderStage stage,
                                 PermutationKey perm, const char* entry = "main") const;

    // Which PermutationKey bits can change this shader's bytecode: a bit is in the mask only if
    // the .hlsl source or its transitive #include closure names the matching macro (bit ->
    // macro map: PERMUTATION_KEY_BITS). Bits outside the mask are stripped before the shader is
    // keyed, so variants differing only in an ignored bit share one cache entry.
    // Generated: PermutationRelevance.generated.h, written by tools/gen_perm_relevance.py from
    // each static id's include closure. verify's perm_relevance gate fails when it is stale, and
    // the first load of each source version re-derives the closure and reports a bit it names
    // that the row lacks ([PERMREL]). An id the generator could not resolve, and every dynamic
    // id (user source that may test anything), keeps all bits.
    // Two callers must agree: KeyBits (the .ishdr key GetShaderInternal looks up) and
    // PSODesc::Hash in PSOCache.cpp (the PSO key and, through psd.cacheKey, the on-disk
    // PSO-library entry name).
    static uint32_t RelevantPermMask(uint32_t id) noexcept;

    // The bits a shader is cached under: RelevantPermMask after a static id's
    // blend bits are canonical (PermutationKey::CanonicalBlend). Every lookup
    // keyed by (id, perm) - the shader, its reflection, its compile error - goes
    // through this one function.
    static uint32_t KeyBits(uint32_t id, PermutationKey perm) noexcept;

    // A dynamic shader's permutation mask: the bits its own files name
    // (MaterialShaderDecl::Decl::permBits) plus the generated rows of the engine sources they
    // #include (PermutationRelevance::kHeaderRows - each one's whole #include closure). The
    // closure rule a static row is made by, computed from the declarations - which a packed
    // build reads too, so it keys its cooked variants exactly as the cook did. Every bit when
    // the source is unread, pastes tokens or has an unresolved include; an engine source
    // without a row adds none (its closure names no bit). KeyBits uses it (cached per registry
    // entry); RelevantPermMask, the PSO identity, keeps every bit for a dynamic id.
    static uint32_t DynamicPermMask(const MaterialShaderDecl::Decl& decl);

    // One line for the process: how many shader closures the first-load check compared with
    // the generated rows (dynamic ones among them), and how many named a bit their mask lacks
    // ("[PERMREL] summary: checked= stale= rows= dynamic= headers="). ~Renderer logs it.
    static void LogRelevanceSummary();

    // The same for the custom-material binding contract: how many surface-material variants
    // had their own resources checked against the two material slots (b8 space0, t* space3),
    // and how many resources sat elsewhere ("[MATSLOT] summary: checked= errors="). ~Renderer
    // logs it.
    static void LogMaterialSlotSummary();

    // The RHI::Shader handle for (@p id, @p perm): compiles and caches on the first call, a
    // lookup afterwards. nullptr on failure.
    const RHI::Shader* GetShader(ShaderID id, PermutationKey perm = {});

    // Dynamic-shader overload. dynId must come from RegisterDynamic.
    const RHI::Shader* GetDynamicShader(uint32_t dynId, PermutationKey perm = {});

    // Reflection for (@p id, @p perm), compiling the shader first if needed (as GetShader does).
    // nullptr when the compile / load failed or reflection rejected the container. The pointer
    // stays valid until a ClearCaches that bumps the reload epoch (GetReloadEpoch), or Shutdown.
    const ShaderReflect::Reflection* GetReflection(ShaderID id, PermutationKey perm = {});
    const ShaderReflect::Reflection* GetDynamicReflection(uint32_t dynId, PermutationKey perm = {});

    // Several permutations of one dynamic shader, reflected and merged: every binding and
    // cbuffer member any of them declares, by name, a member counting as used when any variant
    // reads it - what a material drawing with all of them reads. A resource only one variant
    // keeps (an ALPHA_TEST mask, a forward-only texture) is in it; permutation 0 alone would
    // drop it. Permutations that collapse onto one cache key are one; a single key is just
    // GetDynamicReflection. Cached until a ClearCaches that changes anything; nullptr when
    // none reflects.
    const ShaderReflect::Reflection* GetDynamicReflectionUnion(uint32_t dynId,
                                                               std::initializer_list<PermutationKey> perms);

    // The interface a dynamic shader's source declares (Graphics/MaterialShaderDecl): every
    // resource its own files declare, the cbuffers laid out by the packing rules, the //@
    // annotations - when the parse is exact; nullptr when it is not (the caller keeps the DXIL
    // reflection). Never compiles. Every member counts as used: the source cannot know what a
    // variant reads (GetDynamicReads can). Cached per library; the pointer lives as a
    // reflection's does - until a ClearCaches that bumps the reload epoch, which one that finds
    // the declaration's files edited does.
    const ShaderReflect::Reflection* GetDynamicInterface(uint32_t dynId);
    // The parse behind it, exact or not; null for an id no registry entry backs.
    std::shared_ptr<const MaterialShaderDecl::Decl> GetDynamicDecl(uint32_t dynId);

    // What the variants of a dynamic shader this library compiled so far read, by name: cbuffer
    // members with D3D_SVF_USED, the resources bound. Drives the "not read" mark beside a
    // declared parameter; known = false until one variant compiled here.
    DynamicReads GetDynamicReads(uint32_t dynId) const;

    // What (id, perm) is drawn with, for a report (the material cost table): its source file
    // and entry point, the permutation bits its blob is keyed by (after KeyBits), the shader
    // cache blob it was loaded from or written to, and its reflected instruction count. Warms
    // the shader as GetReflection does. False when it does not compile or reflect (file / entry
    // are still filled for a registered id).
    bool Report(uint32_t id, PermutationKey perm, ShaderReport& out);

    // Explicitly warm up a specific (id, perm) pair at load time.
    bool PreloadShader(ShaderID id, PermutationKey perm = {});

    // Drops every cached shader a reload would load differently, with its reflection, and
    // every compile-failure record (retried on next use). The static and dynamic registry
    // tables (id <-> source path / entry / stage) are kept, so the next GetShader /
    // GetReflection loads the dropped ones again. Hot reload calls this when an .hlsl / .hlsli
    // file changes.
    // "Differently" is LoadOrCompile's own rule: a handle whose .ishdr has not been rewritten
    // since it was loaded, and whose source (or a transitive #include) is not newer than that
    // .ishdr, would come back byte-for-byte, so it is kept. The device's shader pool never
    // recycles a slot (GraphicsDX12::kMaxShaderPool), and re-creating every shader of every
    // library on every reload fills it after ~110 edits. The hot-reload pre-flight rewrites the
    // .ishdr of every recipe an edit reaches (ShaderRecipes::Commit), so exactly those drop.
    // A dropped RHI::Shader handle a caller cached is released (its bytecode freed): callers
    // must re-fetch, typically by also clearing PSOCache and rebuilding the pass's PSOs.
    // Returns true when anything it held changed (a shader dropped, a failure cleared, an
    // edited declaration dropped). False: every PSO built from this library is current -
    // rebuilding it would only retire another PSO slot - and the reload epoch did not move.
    bool ClearCaches();

    // Last DXC error for (id, perm): the diagnostics of the most recent failed compile, or
    // empty when it compiled clean or was never requested. Returned by value, so the caller
    // need not hold the library's mutex. The ShaderLab inspector shows it as a red "Compile
    // Error" panel when an assigned custom shader fails to build.
    std::string GetCompileError       (ShaderID id,    PermutationKey perm = {});
    std::string GetDynamicCompileError(uint32_t dynId, PermutationKey perm = {});

    // Every VARIANT of one dynamic shader at once (Render.explain): how many
    // permutations compiled and how many failed, with the first failure's
    // DXC output. GetDynamicCompileError asks for ONE permutation - the
    // default one - so a custom PS that fails only in the DOUBLE_SIDED or
    // ALPHA_TEST variant a draw actually uses reads as healthy there.
    DynamicShaderStatus GetDynamicShaderStatus(uint32_t dynId) const;

    // ---- Process-wide failed-shader tally (agent harness) ----
    // Shaders compile at runtime, so a clean msbuild proves nothing about them: a broken shader
    // shows up as a black feature at boot, with no error the caller can key on.
    // Static, not per instance: ShaderLibrary is a by-value member of ~68 render passes and
    // subsystems, each with its own m_failedShaders, so a per-instance count would depend on
    // which library the caller holds. One process-wide atomic, maintained at the single insert
    // and clear sites, covers every instance, the dynamic / custom-shader libraries included.
    static std::uint32_t TotalFailedShaders()
    {
        return s_failedShaderTotal.load(std::memory_order_relaxed);
    }

    // Keeps the tally honest across render-graph rebuilds and world reloads, which destroy
    // passes (and their libraries) wholesale. Shutdown() does not clear m_failedShaders, so
    // without this the counter would only rise and keep reporting failures for shaders since
    // fixed.
    ~ShaderLibrary();

private:
    // A static slot and a DynamicShaderRegistry entry share this shape.
    using ShaderEntry = ShaderSourceEntry;

    struct CacheKey
    {
        uint32_t shaderID;
        uint32_t permBits;
        bool operator==(const CacheKey& o) const noexcept
        { return shaderID == o.shaderID && permBits == o.permBits; }
    };
    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const noexcept
        {
            // FNV-1a 64-bit combine
            uint64_t h = 14695981039346656037ull;
            h ^= static_cast<uint64_t>(k.shaderID);  h *= 1099511628211ull;
            h ^= static_cast<uint64_t>(k.permBits);  h *= 1099511628211ull;
            return static_cast<size_t>(h);
        }
    };

    struct LoadStamp
    {
        std::string hlslPath;
        std::string cachePath;
        uint64_t    cacheTime = 0;   // FileWriteTime(cachePath) when the handle was made; 0 = none
    };

    struct InFlight { bool done = false; };

    // See TotalFailedShaders().
    static std::atomic<std::uint32_t> s_failedShaderTotal;

    // Checks a compiled variant of a dynamic shader against its source declarations.
    void CheckDeclaration(uint32_t id, PermutationKey perm, const ShaderReflect::Reflection& refl);

    // Shared GetShader / GetReflection implementation: takes a raw uint32_t so static ShaderID
    // and dynamic ids flow through one path.
    const RHI::Shader* GetShaderInternal(uint32_t id, PermutationKey perm);
    const ShaderReflect::Reflection* GetReflectionInternal(uint32_t id, PermutationKey perm);

    // Returns the registry entry for a shader id (static or dynamic); nullptr
    // if the id is out of range or not registered. A dynamic entry is the
    // DynamicShaderRegistry's, stable for the process.
    const ShaderEntry* GetRegistryEntry(uint32_t id) const;

    // Internal: compile (or load cached) bytecode and upload to GPU.
    // outBytecode receives the raw DXIL container so the caller (GetShaderInternal)
    // can feed it to the shader reflection layer without re-reading the cache
    // blob. Returns true on success; outShader, outBytecode and outStamp are set.
    bool LoadOrCompile(uint32_t id, PermutationKey perm,
                       RHI::Shader& outShader,
                       std::vector<uint8_t>& outBytecode,
                       LoadStamp& outStamp);

    // The .ishdr cache path for a source + permutation; forwards to ShaderDiskCache::CachePath.
    // The key holds the stage (a unified single-file material compiles VS and PS at the same
    // (entry, perm)), a compile-flags tag (Debug `-Zi -Od` vs Release `-O3` share this
    // directory) and a hash of the full source path (absolute custom-shader paths flatten to
    // their file name). Not in the key: the DXC compiler version, so a DXC upgrade alone does not
    // invalidate a cached blob (see LoadOrCompile's cache write).
    std::string CachePath(const std::string& hlslFile, PermutationKey perm,
                          const std::string& entryPoint, RHI::ShaderStage stage) const;

    // Reads @p path (an .ishdr or an .hlsl source) through AssetFS: the pak when mounted, else
    // the disk. Empty when the file is not found.
    static std::vector<uint8_t> ReadFile(const std::string& path);

    // Writes @p data to @p path atomically (temporary sibling + rename), creating the directory.
    static bool WriteFile(const std::string& path, const std::vector<uint8_t>& data);

    // The DXC define list for @p perm: one NAME = "0" / "1" pair per PermutationKey bit
    // (PERMUTATION_KEY_BITS).
    static std::vector<std::pair<std::string,std::string>> MakeDefines(PermutationKey perm);

    std::string      m_shaderDir;
    // Where compiled .ishdr blobs land: inside shaders/, apart from the .hlsl sources.
    // CachePath strips shaderDir from the source's parent, so "shaders/GBuffer.ps.hlsl" caches
    // to "shaders/shader_cache_dxil/GBuffer.ps_main_S1_P00000001_O3H21_H1A2B3C4D.ishdr" (entry,
    // stage, permutation, compile-flags tag, source-path hash). The _dxil name keeps DXC (DXIL,
    // SM 6.6) blobs apart from D3DCompile DXBC caches, which a SM 6.6 PSO cannot load.
    std::string      m_cacheDir = "shaders/shader_cache_dxil/";
    IGraphicsDevice* m_gfx = nullptr;

    // The static slots, indexed by ShaderID. Dynamic entries are not here: DynamicShaderRegistry
    // holds them for every library, and their ids key the cache maps below like a static id does.
    ShaderEntry m_registry[static_cast<uint32_t>(ShaderID::Count)];

    std::unordered_map<CacheKey, RHI::Shader, CacheKeyHash> m_shaderCache;
    // Parallel map keyed the same way. Separate from m_shaderCache so the
    // hot per-frame GetShader path stays tight; reflection is only consulted
    // by editor UI / material binding setup.
    std::unordered_map<CacheKey, ShaderReflect::Reflection, CacheKeyHash> m_reflectionCache;
    // GetDynamicReflectionUnion's merged reflections, by (id, sorted cache keys).
    std::map<std::pair<uint32_t, std::vector<uint32_t>>, ShaderReflect::Reflection> m_reflectionUnions;
    // GetDynamicDecl's parses, by dynamic id: what GetDynamicInterface hands out stays alive
    // here until ClearCaches drops an edited one (or Shutdown).
    std::unordered_map<uint32_t, std::shared_ptr<const MaterialShaderDecl::Decl>> m_interfaces;

    // What each m_shaderCache handle was loaded from - how ClearCaches tells a
    // handle a reload would load again unchanged (kept) from one it would not.
    std::unordered_map<CacheKey, LoadStamp, CacheKeyHash> m_loadStamps;
    // Negative cache: (id, perm) -> DXC error of the last failed compile. GetShaderInternal
    // short-circuits on a present key, so a broken shader does not re-run DXC every frame and
    // spam the log. ClearCaches drops it with the others, so a hot reload (an edited source)
    // gives the shader another chance.
    std::unordered_map<CacheKey, std::string, CacheKeyHash> m_failedShaders;

    // One load / compile per (id, perm) at a time: a thread that misses a key another thread
    // is already building waits for that build instead of making a twin, which would retire a
    // shader-pool slot for good (the backend's pool is append-only) and double the DXC time.
    // Under m_mutex.
    std::unordered_map<CacheKey, std::shared_ptr<InFlight>, CacheKeyHash> m_inFlight;
    std::condition_variable m_inFlightCv;

    // Guards the maps above (the caches, m_failedShaders, m_inFlight); m_inFlightCv waits on it.
    mutable std::mutex m_mutex;
    std::atomic<uint32_t> m_reloadEpoch{ 1 };   // see GetReloadEpoch()
};
