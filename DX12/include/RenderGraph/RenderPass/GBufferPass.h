#pragma once

// GBufferPass - PBR geometry pass writing albedo, normals, surface data, velocity,
// emissive and depth. LightingPass reconstructs world position from depth.
//
// - Owns a ShaderLibrary (GBuffer_VS / GBuffer_PS) and a PSOCache; the library is
//   reached through a pointer that defaults to the owned one, so a second instance
//   can borrow the first's (SetSharedShaderLibrary).
// - Picks the PSO per draw from DrawPacket::permutation (PSOCache::GetOrCreate) and
//   batches draws that share one (the draw list is sorted by permutation).
// - Knows no scene geometry: all mesh data comes from DrawPackets.
// - Binds the per-frame MaterialBuffer (StructuredBuffer<MaterialGPUData>) at t2 space0.
//
// Parallel recording: PrepareRecording allocates up to kMaxChunks ordered CLs from
// the draw count and free workers; the primary CL clears and records chunk 0, the
// others are flat RenderGraph recording tasks. A direct Execute records one CL.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/MaterialSampler.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/FrameConstants.h"   // RHI::kFramesInFlight (the statistics readback ring)

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace RG { class RenderContext; }

class GBufferPass : public RG::RenderPass
{
public:
    // The base name every instance's GetName() starts with, kept in one place.
    static constexpr const char* kBaseName = "GBufferPass";

    struct MeshletGpuStats
    {
        bool     valid = false;
        uint32_t frame = 0;          // the frame that recorded them (SetMeshletFrame)
        uint32_t counters[16] = {};
    };

    struct MeshletFrameStats
    {
        bool     active        = false;  // the latch this pass recorded with
        bool     indirect      = false;  // the frame took the ExecuteIndirect path
        uint32_t packets       = 0;      // packets drawn through the mesh pipeline
        uint32_t instances     = 0;
        uint64_t asGroups      = 0;      // sum of blocks * instances dispatched
        uint32_t limitFallback = 0;      // eligible but past the DispatchMesh limits -> VS
        uint32_t psoFallback   = 0;      // eligible but the mesh PSO was unavailable -> VS
        uint32_t prepassPackets = 0;     // meshlet packets in the CPU depth prepass
        // The ExecuteIndirect path: meshlet groups executed with the DISPATCH_MESH
        // signature, their command capacity (the GPU draws the survivors), and meshlet
        // groups whose mesh PSO was unavailable - drawn on the CPU through the VS.
        uint32_t indirectGroups = 0;
        uint32_t indirectCmds   = 0;
        uint32_t groupFallback  = 0;
    };

    // Minimum number of draws per chunk. Below this threshold the pass records
    // everything on a single CL to avoid TaskSystem overhead.
    static constexpr int kMinDrawsPerChunk = 16;
    // Maximum number of parallel recording chunks (= parallel command lists).
    static constexpr int kMaxChunks        = 4;

    GBufferPass(RG::RGTextureHandle albedo,
                RG::RGTextureHandle normal,
                RG::RGTextureHandle surface,
                RG::RGTextureHandle depth,
                RG::RGTextureHandle velocity,
                RG::RGTextureHandle emissive);

    // Instance name. RenderGraph keys the external-wait map, the PIX/RenderDoc marker
    // and the GPU timestamp region on GetName(), and GetGPUTimestampResult returns the
    // first match of a linear name scan, so two instances must not share a name. With
    // no suffix it is kBaseName, which Renderer's SetExternalWait keys expect.
    const char* GetName() const override { return m_name.c_str(); }
    // Call before the pass is handed to a RenderGraph: the name keys waits and
    // regions registered under it, so renaming a live pass silently drops them.
    void SetNameSuffix(const char* suffix);

    void Setup  (RG::RenderGraphBuilder& b)           override;
    void Init   (IGraphicsDevice& gfx)                override;
    // Releases what THIS instance created. A borrower must not touch the shared
    // library, so the library half is gated on OwnsShaderLibrary(), as Init and
    // ReloadShaders are.
    void Shutdown(IGraphicsDevice& gfx)               override;
    // Renderer::PassSet delivers Shutdown: the main graph never Destroys its
    // passes, so this is their only Shutdown.
    // kViewSafe: it draws the view's list into the view's targets with the view's constants.
    uint32_t HandledEvents() const override { return kShutdown | kViewSafe; }
    RHI::CommandList Execute(RHI::CommandList cl) override;
    uint32_t PrepareRecording(RHI::CommandList cl) override;
    RHI::CommandList ExecutePrepared(RHI::CommandList cl) override { return Execute(cl); }
    void ExecuteAdditionalRecording(uint32_t task) override;
    void FinishRecording() override;
    void ReloadShaders(IGraphicsDevice& gfx)          override;

    // ExecuteIndirect path: set by Renderer each frame when GPU culling is active.
    // @p countBuf holds per-GROUP survivor-command counts, one u32 per group in
    // m_indirectGroups order (group i at byte i*4); ExecuteIndirect draws
    // min(count, cmdCount) commands. Null = CPU-exact cmdCount, degenerate slots zeroed.
    void SetIndirectDraw(const RHI::GPUBuffer* argBuf,
                         const std::vector<IndirectGroup>& groups,
                         const RHI::GPUBuffer* countBuf = nullptr)
    {
        m_indirectArgBuffer   = argBuf;
        m_indirectGroups      = groups;
        m_indirectCountBuffer = countBuf;
    }
    void ClearIndirectDraw()
    {
        m_indirectArgBuffer   = nullptr;
        m_indirectCountBuffer = nullptr;
        m_indirectGroups.clear();
    }

    // Global view mode: true rasterizes with FILL_MODE_WIREFRAME. BuildPSODesc bakes
    // it into the PSO key (PSOCache hashes rs.fillMode), so wireframe gets its own
    // PSO variant. Set per frame by Renderer from ViewMode::Wireframe.
    void SetWireframe(bool w) { m_wireframe = w; }
    // The DEBUG_SHADE permutation on this frame's GBuffer PSOs (BuildPSODesc: the
    // vertex and meshlet paths; the depth prepass has no pixel shader), for a frame
    // that shows or captures a debug target the pixel shaders count into. Per frame.
    void SetDebugShade(bool on) { m_debugShade = on; }

    // ---- Meshlet path ----
    // An eligible packet (DrawPacket::meshletBlocks != 0, see MeshletEligible) draws
    // through SceneMeshlet.as/.ms + the unchanged GBuffer.ps instead of GBuffer.vs:
    // DispatchMesh(blocks, instanceCount, 1) with MeshletCullCB at b4. Both the CPU
    // per-draw path (RecordChunk, the CPU depth prepass) and the ExecuteIndirect path
    // take it: a group the Renderer keyed meshlet (commands converted to DISPATCH_MESH
    // by the cull CS) runs with the mesh PSO through ExecuteIndirectDispatchMesh.
    //
    // CAPABLE: set by the Renderer on the MAIN instance, before Init, when the device
    // has mesh shaders. Only a capable instance registers the AS / MS pre-warm and
    // builds mesh PSOs, so a machine without them (or -force_no_mesh_shader) creates none.
    void SetMeshletCapable(bool v) { m_meshletCapable = v; }
    // The mesh PSO pre-warmed in Init / ReloadShaders. The Renderer's path latch
    // requires it; a PSO that fails later at record time clears it (next frame
    // the path is off, reason pso_failed).
    bool MeshletPsoReady() const { return m_meshletPsoReady.load(std::memory_order_acquire); }
    // Per frame, from the render half: SceneEmit's latch for THIS frame.
    void SetMeshletFrame(bool active, uint32_t frame)
    {
        m_meshletFrame       = active && m_meshletCapable;
        m_meshletFrameNumber = frame;
    }
    // Per frame, from the render half: the Meshlets / Meshlet LOD view mode -
    // every opaque packet draws with MeshletDebug.ps.
    void SetMeshletDebugView(bool on) { m_meshletDebugView = on; }
    // The statistics buffer's heap index for MeshletCullCB.statsUavIdx
    // (0xFFFFFFFF when the buffers could not be created). Per frame.
    uint32_t MeshletStatsUavIndex(IGraphicsDevice& gfx) const;

    // The last statistics read back from the GPU (16 counters, the
    // meshlet_shared.hlsli kMeshletStat* layout), ~kFramesInFlight frames old.
    MeshletGpuStats GetMeshletGpuStats() const
    {
        std::lock_guard<std::mutex> lk(m_meshletStatsMutex);
        return m_mlGpuStatsLast;
    }
    // The eligibility a packet needs on top of the latch (the path half; a custom
    // PS is allowed). The geometric half (library mesh with an MLET, not skinned, no
    // custom VS) was decided at gather and is meshletBlocks != 0.
    static bool MeshletEligible(const DrawPacket& dp);
    // The 4th root constant of a meshlet draw (the AS reads the no-cone bit,
    // MeshletDebug.ps the meshlet bit). The Renderer writes it into a meshlet
    // group's indirect commands.
    static uint32_t MeshletDrawFlags(const DrawPacket& dp);
    // A custom PS whose mesh PSO failed to link: it keeps the VS for the rest
    // of the run (the Renderer keys no meshlet group for it; any thread).
    bool MeshletPsBlocked(uint32_t customPSID) const
    {
        std::lock_guard<std::mutex> lk(m_meshletBlockedPsMutex);
        return m_meshletBlockedPs.count(customPSID) != 0;
    }
    // D3D12 limits for one DispatchMesh: every dimension <= 65535 and
    // X*Y*Z <= 2^22. A packet past them draws through the VS (counted).
    static bool MeshletDispatchFits(uint32_t blocks, uint32_t instances)
    {
        return MeshletDispatchWithinLimits(blocks, instances);   // RenderTypes.h
    }

    // What the last recorded frame did on the meshlet path, snapshot when its
    // recording finished (FinishRecording / the direct Execute) so a reader on
    // another thread never sees a half-recorded frame.
    MeshletFrameStats GetMeshletFrameStats() const
    {
        std::lock_guard<std::mutex> lk(m_meshletStatsMutex);
        return m_meshletStatsLast;
    }
    // Set before Init(). 1 creates a trilinear sampler; 2..16 create an
    // anisotropic sampler with the requested maximum.
    void SetTextureAnisotropy(uint32_t x) { m_textureAnisotropy = x; }
    // The s0 material sampler's LOD bias follows the upscale ratio
    // (Graphics/MaterialSampler.h). Renderer::BeginFrame calls it with the graph's
    // resize recompile, never while a frame records.
    void SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW);
    float GetMipBias() const { return m_mipBias; }

    // The ShaderLibrary this instance compiles through - the BORROWED one on a
    // borrowing instance (upstream code reflects material custom shaders against it
    // and asks it SourceDeclares). Dynamic ids come from DynamicShaderRegistry, so
    // any library resolves them.
    ShaderLibrary& GetShaderLibrary() { return *m_lib; }

    // ---- ShaderLibrary injection ----
    // A second instance (the preview graph's) compiles through the main instance's
    // library, so a custom material's shaders compile and hot-reload once for both.
    // Call before Init(): Init wires m_psoCache to the library, and only the OWNER
    // registers the two static GBuffer shaders.
    // Borrowing obliges:
    // - ORDER: the borrower Init()s after the owner (the owner calls
    //   ShaderLibrary::Init + Register).
    // - HOT RELOAD: never ClearCaches() a borrowed library (a second clear
    //   double-bumps the reload epoch MaterialReflectionSync stamps against).
    //   ReloadShaders clears it only when OwnsShaderLibrary(), and this class does
    //   not override GetReloadableShaderLibrary() (base: nullptr), so the graph's
    //   default reload cannot reach it either.
    // - LIFETIME: PSOCache keeps a bare ShaderLibrary*; a borrower must not outlive
    //   the owner.
    // - SIGNATURE: only for GBuffer-shaped opaque output. A custom PS's SV_Target
    //   count comes from its permutation bits, so a translucent / water / front-layer
    //   material must borrow that domain's library.
    void SetSharedShaderLibrary(ShaderLibrary& lib) { m_lib = &lib; }
    bool OwnsShaderLibrary() const { return m_lib == &m_ownedLib; }

    // ---- Fallback textures and PSOs ----
    // The 1x1 white SRV other passes borrow as a fallback texture (e.g. probe
    // capture's t3 base-colour slot when a draw has no bindless map).
    uint64_t GetDefaultWhiteSrvHandle()      const { return m_defaultWhiteGpuHandle; }
    // The 1x1 white texture itself - renderer-lifetime (created at Init, never
    // scene-scoped or deferred-released). Fills unwritten custom-material space3
    // table slots so they never sample a freed descriptor.
    const RHI::Texture& GetDefaultWhiteTexture() const { return m_defaultWhite; }
    // 1x1 flat-normal SRV (RGB = 0.5, 0.5, 1.0), the neutral tangent-space normal:
    // the t5 normal-map fallback when a draw lacks one.
    uint64_t GetDefaultFlatNormalSrvHandle() const { return m_defaultFlatNormalGpuHandle; }
    // The renderer-lifetime 1x1 texture an UNASSIGNED custom-material slot of this
    // kind samples (ShaderReflect::TextureDefault, from the declaration's //@default):
    // white, black (0,0,0,1), grey (0.5 as sampled, or sRGB mid-grey for an @srgb
    // slot) or the flat normal. Only the shader library's owner creates the typed
    // ones; a borrowing instance - and a failed creation - answers white.
    const RHI::Texture& GetDefaultTexture(ShaderReflect::TextureDefault kind,
                                          ShaderReflect::TextureColorSpace space) const;

    // Public on purpose: SurfaceCacheBakePass re-issues the SAME GBuffer draw into UV
    // space, so its PSO must be this desc with only four overrides (bake VS, RGBA
    // rtvFormats[0], no depth/stencil, cull NONE); reading it from here keeps the
    // two in lockstep. @p customPSID / @p customVSID: 0 keeps GBuffer_PS / GBuffer_VS;
    // any other value is a shader id - a dynamic one (DynamicShaderRegistry) or a
    // static ShaderID such as MeshletDebug_PS - that goes into the PSO key as is.
    // @p mirrored: the instances have a negative-determinant world matrix - sets
    // rs.frontCounterClockwise so BACK cull still culls the back face.
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customPSID = 0, uint32_t customVSID = 0,
                         bool mirrored = false) const;

    // Builds now, on the calling thread (a preload's TaskSystem worker), the PSO
    // Execute would build for @p dp - a material's synthetic packet. Nothing is bound
    // or drawn; the PSO cache is thread-safe (single flight). Only the vertex path's
    // PSO: a skinned packet never takes the meshlet path, and a static
    // meshlet-eligible packet's mesh PSO is not warmed.
    void WarmPipelines(const DrawPacket& dp);

    // ---- Depth prepass ----
    void SetDepthPrepassEnabled(bool v) { m_depthPrepassEnabled = v; }
    bool IsDepthPrepassEnabled() const  { return m_depthPrepassEnabled; }
    uint32_t GetDepthPrepassDrawCount() const { return m_depthPrepassDraws; }

private:
    PSODesc  BuildDepthPrepassPSODesc(PermutationKey perm, uint32_t customVSID,
                                      bool mirrored) const;
    RHI::CommandList ExecuteDepthPrepass(RHI::CommandList cl, const DrawList& draws);

    // Bind all per-frame global resources onto @p cl (heaps, CBs, SRVs, samplers).
    // Called once per PSO switch inside RecordChunk.
    void BindGlobals(RHI::CommandList cl, uint64_t bindlessHandle) const;

    // ---- Meshlet path (see SetMeshletCapable) ----
    // The VS descs with the mesh stages swapped in: same raster / depth / stencil /
    // blend state, so the two paths differ only in who makes the vertices (and the
    // prepass stays depth-identical to its GBuffer draw).
    PSODesc BuildMeshletPSODesc(PermutationKey perm, uint32_t customPSID, bool mirrored) const;
    PSODesc BuildMeshletDepthPrepassPSODesc(PermutationKey perm, bool mirrored) const;
    // Builds the default mesh PSO; sets m_meshletPsoReady. No-op (false) unless capable.
    bool PrewarmMeshletPSO();
    // Resolves the PSO a packet draws with on the meshlet path, or nullptr when
    // it must take the VS path (counted into the frame's fallbacks).
    const RHI::PipelineState* MeshletPSOFor(const DrawPacket& dp, bool depthPrepass);
    // The mesh PSO of (perm, custom PS, mirrored) - MeshletPSOFor's and the
    // indirect groups' common core; nullptr = the VS must draw it.
    const RHI::PipelineState* MeshletPso(PermutationKey perm, uint32_t customPSID, bool mirrored,
                                         bool depthPrepass);
    // A meshlet group whose mesh PSO is unavailable: its working args are
    // DISPATCH_MESH-shaped, so they cannot run as DRAW - its packets draw on
    // the CPU through the VS, every instance (no GPU culling for it this frame).
    void DrawMeshletGroupOnCpu(RHI::CommandList cl, const IndirectGroup& g, bool depthPrepass,
                               uint64_t bindlessHdl);
    // Whether this frame's indirect groups include a meshlet group.
    bool AnyMeshletGroup() const;
    void ResetMeshletCounters();
    void SnapshotMeshletCounters(bool indirect);
    void CreateMeshletStatsBuffers(IGraphicsDevice& gfx);
    void DestroyMeshletStatsBuffers(IGraphicsDevice& gfx);
    void HarvestMeshletStats(IGraphicsDevice& gfx);
    void BeginMeshletStats(RHI::CommandList cl);   // primary CL, before any draw
    void EndMeshletStats(RHI::CommandList cl);     // the last chunk CL, after every draw

    // ---- Parallel chunk recording ----
    // Records the slice [begin, end) of @p draws onto @p cl. Safe on several TaskSystem
    // workers at once as long as each uses a distinct CL: they share m_psoCache
    // (internally locked) and only read the default texture handles and
    // m_linearSamplerIdx. @p lastChunk: this CL is the frame's last chunk - the
    // statistics copy rides it.
    void RecordChunk(RHI::CommandList cl, DrawList draws, size_t begin, size_t end, bool lastChunk);

    bool m_recordPrepared = false;
    uint32_t m_recordChunkCount = 1;
    DrawList m_recordDraws{};
    RHI::CommandList m_recordCLs[kMaxChunks]{};

    // ---- Depth prepass ----
    // A depth-ONLY pass over the draws that cannot discard, so the GBuffer pass
    // early-Z rejects occluded fragments instead of shading them into 6 MRTs
    // (~28 B/pixel per overdrawn fragment).
    // The GBuffer keeps GREATER_EQUAL with depth write on, not EQUAL: same vsID +
    // same raster state gives identical depth, and a draw the prepass skipped still
    // renders. EQUAL's failure mode is whole objects vanishing.
    bool     m_depthPrepassEnabled = false;
    uint32_t m_depthPrepassDraws   = 0;   // last frame's covered-draw count

    // ---- Meshlet path (see SetMeshletCapable) ----
    bool                  m_meshletCapable = false;
    std::atomic<bool>     m_meshletPsoReady{ false };
    bool                  m_meshletFrame   = false;   // this frame's latch (render half)
    uint32_t              m_meshletFrameNumber = 0;   // its frame number (render half)
    bool                  m_meshletDebugView = false; // the Meshlets view mode (render half)
    // -meshletgroupcpu (harness): every indirect meshlet group takes
    // DrawMeshletGroupOnCpu - the path a group whose mesh PSO is unavailable
    // takes - so a pack can prove that fallback draws the same picture.
    bool                  m_forceMeshletGroupCpu = false;
    // Statistics (see CreateMeshletStatsBuffers).
    RHI::GPUBuffer        m_mlStatsBuffer;
    RHI::GPUBuffer        m_mlStatsReadback[RHI::kFramesInFlight];
    std::atomic<bool>     m_mlStatsPending[RHI::kFramesInFlight] = {};
    uint32_t              m_mlStatsFrame[RHI::kFramesInFlight] = {};   // the frame each slot's copy holds
    bool                  m_mlStatsArmed = false;     // this frame zeroed the buffer (render half)
    MeshletGpuStats       m_mlGpuStatsLast;           // guarded by m_meshletStatsMutex
    // Recording counters: chunks record on workers, hence atomics.
    std::atomic<uint32_t> m_mlPackets{ 0 };
    std::atomic<uint32_t> m_mlInstances{ 0 };
    std::atomic<uint64_t> m_mlAsGroups{ 0 };
    std::atomic<uint32_t> m_mlLimitFallback{ 0 };
    std::atomic<uint32_t> m_mlPsoFallback{ 0 };
    std::atomic<uint32_t> m_mlPrepassPackets{ 0 };
    std::atomic<uint32_t> m_mlIndirectGroups{ 0 };
    std::atomic<uint32_t> m_mlIndirectCmds{ 0 };
    std::atomic<uint32_t> m_mlGroupFallback{ 0 };
    mutable std::mutex    m_meshletStatsMutex;
    MeshletFrameStats     m_meshletStatsLast;
    // Custom PS ids whose mesh PSO failed to build: they keep the VS path.
    mutable std::mutex           m_meshletBlockedPsMutex;
    std::unordered_set<uint32_t> m_meshletBlockedPs;

    // ---- Shaders, name, targets and draw state ----
    // The library this instance owns. Empty and untouched on a borrowing
    // instance (kept as a member so ownership is a pointer comparison rather
    // than a flag that can disagree with reality).
    ShaderLibrary       m_ownedLib;
    // Every library access goes through this; defaults to the owned one.
    ShaderLibrary*      m_lib = &m_ownedLib;
    PSOCache            m_psoCache;

    // See GetName() / SetNameSuffix.
    std::string         m_name = kBaseName;
    // Logs ONE line the first time this instance builds a PSO desc that carries a
    // custom shader id, proving the id survived the request -> PSO-key hop. Mutable:
    // BuildPSODesc is const and runs on recording workers as well as Init; the atomic
    // exchange keeps two racing chunks to one line.
    mutable std::atomic<bool> m_customPsoLogged{ false };
    RG::RGTextureHandle m_albedo;
    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_velocity;
    RG::RGTextureHandle m_emissive;

    // Default 1x1 white texture (fallback for base-color and surface map slots).
    RHI::Texture m_defaultWhite;
    uint64_t     m_defaultWhiteGpuHandle = 0;

    // Default 1x1 flat-normal texture (RGB=0.5,0.5,1.0 -> tangent-space (0,0,1)).
    RHI::Texture m_defaultFlatNormal;
    uint64_t     m_defaultFlatNormalGpuHandle = 0;

    // The other typed fallbacks an unassigned custom-material slot can ask for
    // (GetDefaultTexture).
    RHI::Texture m_defaultBlack;
    RHI::Texture m_defaultGrey;
    RHI::Texture m_defaultGreySRGB;

    // Linear-wrap sampler bound at s0 space0 for texture sampling in the PS,
    // and the LOD bias it was built with (SetUpscaleMipBias).
    int   m_linearSamplerIdx = -1;
    float m_mipBias          = MaterialSampler::kNativeBias;
    uint32_t m_textureAnisotropy = 8;

    // ExecuteIndirect path (set per-frame by Renderer).
    const RHI::GPUBuffer*           m_indirectArgBuffer   = nullptr;
    const RHI::GPUBuffer*           m_indirectCountBuffer = nullptr; // null = CPU-exact counts
    std::vector<::IndirectGroup>    m_indirectGroups;

    // Global view-mode wireframe toggle (see SetWireframe). Atomic: set by the
    // render half, read by BuildPSODesc on a recording worker and on a preload's
    // worker (WarmPipelines).
    std::atomic<bool>               m_wireframe{ false };
    std::atomic<bool>               m_debugShade{ false };   // see SetDebugShade
};
