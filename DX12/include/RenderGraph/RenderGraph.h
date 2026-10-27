#pragma once

// RenderGraph: transient resource manager, barrier scheduler and pass executor.
//
// Responsibility boundary:
//   - allocates / destroys GPU textures via IGraphicsDevice::CreateTexture;
//   - computes and emits resource barriers before each pass via
//     IGraphicsDevice::PushBarrier;
//   - builds a RenderContext per frame and hands it to each pass's Execute;
//   - contains no draw call logic, but carries scene data by pointer:
//     SetDrawList / m_drawList hold a borrowed const std::vector<DrawPacket>*;
//   - no DX12 type names appear in this header, yet it is not backend-neutral:
//     SetBindlessTableHandle(uint64_t) stores a raw
//     D3D12_GPU_DESCRIPTOR_HANDLE::ptr from the DX12 backend.

#include "System/OwnerTask.h"
#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/Stage.h"
#include "RenderGraph/Order.h"    // Order, OrderRule
#include <array>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ViewSlot.h"          // RHI::kMaxViews
#include "RenderGraph/GraphSnapshot.h"
#include "RenderGraph/ImportSpec.h"      // an import's source / entry / tracking
#include "RenderGraph/RenderGraphDag.h"   // the per-frame DAG + validators
#include "RenderGraph/RenderContext.h"

class TransientAliasArena;

struct DrawPacket;    // full type in Graphics/RenderTypes.h

class IGraphicsDevice;
class ShaderLibrary;
class PSOCache;

namespace RG
{
    enum class BuiltinTexture
    {
        SwapChainColor,
        HdrSceneColor,           // auto-bind + CLEAR: the pass is the first HDR writer
        HdrSceneColorPreserve,   // auto-bind, NO clear: the pass is a downstream writer
                                 // (e.g. LightingPass, additive on top of GBufferPass's
                                 // emissive seed). Same state transitions as HdrSceneColor;
                                 // only the clear is skipped.
        None,   // Pass handles its own render target setup inside Execute.
    };

    // The HDR scene colour's name in a graph that imports it
    // (RenderGraph::ImportTexture): the main graph and every preview graph import
    // their own slot's target under it, so a pass shared by them finds it in Setup
    // by this one name (RenderGraphBuilder::GetTexture).
    inline constexpr const char* kHdrSceneTexture = "HdrScene";

    // A render-target declaration's load op (RenderGraphBuilder::RenderTarget):
    //   Load     - the pass draws on top of what is there (every
    //              WriteRenderTarget, and every HDR writer after the first);
    //   Clear    - the content starts from the clear value (an import's
    //              BindImport value); only the BuiltinTexture::HdrSceneColor shim
    //              declares it, and its Phase A bind clears;
    //   DontCare - the pass overwrites every pixel it needs; nothing is kept
    //              and nothing is cleared.
    // The DAG reads it (a Clear on a texture already written this frame is a
    // ClearAfterLive finding); the barrier is the same for all three.
    enum class Load : uint8_t { Load, Clear, DontCare };

    // What a pass does to each plane of a depth texture through its DSV
    // (RenderGraphBuilder::DepthStencil) - and to a plane of one, for an SRV or a
    // copy. Write: the plane is written (DEPTH_WRITE). Read: only tested - the
    // plane stays where it is when that state permits the test (DEPTH_WRITE, or a
    // DEPTH_READ state), else it moves to DEPTH_READ_SRV. None: the DSV is bound
    // but the plane is not used; it is kept in a state a DSV binding takes, as
    // for Read.
    enum class Depth   : uint8_t { None, Read, Write };
    enum class Stencil : uint8_t { None, Read, Write };
    enum class Plane   : uint8_t { Depth = 0, Stencil = 1 };

    class RenderPass;
    class RenderGraphBuilder;

    // ==== Pass activity escape hatch ====
    // An IsActive() override that opens with
    //     return !RG::PassActivityGatesEnabled() || <predicate>;
    // is "always true" under -passactiveoff, the A/B of what the gates skip;
    // overrides without that prefix are not affected. RTAOPass / XeGTAOPass
    // deliberately leave the prefix out: they are MUTUALLY EXCLUSIVE (the Renderer
    // activates one and deactivates the other), so forcing both true would change
    // what renders, not just what it costs. A lambda pass's isActiveFn needs no
    // prefix and cannot opt out: the graph wraps every non-empty one in the escape
    // (LambdaRenderPass::IsActive). Resolved once from the command line on first
    // query.
    // SetPassActivityGatesEnabled is an INSTRUMENT, not a runtime knob: the
    // [RG_ACTIVE] line and -passactiveverify switch the gates off in
    // RenderGraph::Execute's serial Phase A for one extra activity sweep, to report
    // the before / after counts from a single run. The flag is a plain,
    // unsynchronised bool, and IsActive() is also queried outside Phase A
    // (Renderer::ReadPlanState asks TransparentPass, ParticleRenderPass and
    // FaceOverlayPass, on the game half too, which can overlap the render half), so
    // such a query running beside that sweep can see the gates off and answer true.
    bool PassActivityGatesEnabled();
    void SetPassActivityGatesEnabled(bool on);

    // ==== Physical residency of a graph-scope texture (CreateTexture's 3rd argument) ====
    // Always   - backed by every Compile() for the life of the graph.
    // OnDemand - DECLARED exactly like an Always texture (stable handle, Setup
    //            reads / writes, bind flags derived from them) but not BACKED
    //            until its producer asks with EnsurePhysicalTexture(). Until then
    //            it holds no memory and no descriptor: GetPhysicalTexture()
    //            returns nullptr and the barrier emitter skips it, as for an
    //            Always texture whose CreateTexture failed. For a render-sized
    //            target whose producer is off by default, e.g. the ReSTIR DI / GI
    //            and path-tracer results (Renderer::Compile): CreatePhysicalTextures
    //            has no liveness cull, so an Always target is paid for from boot
    //            whether or not anything ever writes it.
    enum class TextureResidency : uint8_t { Always, OnDemand };

    // An import's spec - source, entry, tracking - is RenderGraph/ImportSpec.h;
    // a pass hands its own over through this.
    class ImportDeclarer;
    // A history's two imports (RenderGraph/PassTexture.h).
    class HistoryTexture;
    struct HistoryHandles
    {
        RGTextureHandle cur;    // the texture this frame writes
        RGTextureHandle prev;   // the one written last
    };

    class RenderGraph
    {
    public:
        struct DestroyStats
        {
            uint32_t textures = 0;   // physical textures handed to the deferred bucket
            uint32_t passes   = 0;   // pass instances destroyed
        };

        struct PassInfo
        {
            std::string              name;
            bool                     ran     = false;
            bool                     shown   = true;
            const char*              colorTarget = "None";
            const char*              stage       = "Unstaged";   // RG::StageName of its AddPass tag
            std::vector<std::string> reads;         // ReadSRV
            std::vector<std::string> writes;        // render targets
            std::vector<std::string> depth;         // depth-stencil
            std::vector<std::string> readWrite;     // UAV
            std::vector<std::string> manual;        // self-barriered
            std::vector<std::string> scoped;        // ReadScoped
        };

        struct TextureInfo
        {
            std::string name;
            uint32_t    width = 0, height = 0;   // 0 = the render size
            uint32_t    format = 0;              // RHI::Format value
            uint32_t    arraySize = 1;
            bool        isDepth = false, isUAV = false;
            bool        onDemand = false, demanded = false;
            bool        imported = false;   // ImportTexture or a pass's import
            ImportEntry    entry    = ImportEntry::Stale;      // imported only
            ImportTracking tracking = ImportTracking::Graph;   // imported only
        };

        // ArmTextureCopy's result (TakeTextureCopyResult).
        struct TextureCopyResult
        {
            bool        armed    = false;   // an arm reached an Execute
            bool        done     = false;   // the copy is recorded
            bool        deferred = false;   // a snapshot frame: nothing recorded - arm again
            std::string afterPass;          // the pass it followed (the last one's name for "")
            uint64_t    frame    = 0;       // SetSnapshotFrame's stamp
            std::string why;                // why not done
        };

        ~RenderGraph();

        // ---- Class-based pass registration ----
        // @p stage (RenderGraph/Stage.h) says where in the frame the pass belongs,
        // @p order (RenderGraph/Order.h) where among its stage's passes when its
        // registration position should not decide it. Compile's first sort turns the
        // registration order into the declaration order the graph runs in: stage
        // first, inside a stage the constraints, then the registration order. The
        // overloads without a stage add an Unstaged pass, which keeps its position
        // (the preview graphs, the harness's test passes); the main graph tags every
        // pass.
        void AddPass(Stage stage, std::unique_ptr<RenderPass> pass,
                     std::initializer_list<Order> order = {});
        void AddPass(std::unique_ptr<RenderPass> pass);

        // ---- Lambda-based pass registration ----
        // @p isActiveFn is the lambda form of RenderPass::IsActive and obeys the same
        // contract (serial Phase A, side-effect free, reads only state already
        // settled - see the base declaration). Omitted or empty = always active. The
        // graph wraps a non-empty one in the -passactiveoff escape, so the lambda
        // needs no PassActivityGatesEnabled() prefix of its own and cannot opt out
        // of it.
        void AddPass(Stage stage, const char* name,
                     std::function<void(RenderGraphBuilder&)>   setupFn,
                     std::function<void(RHI::CommandList&)>     executeFn,
                     std::function<bool()>                      isActiveFn = {},
                     std::initializer_list<Order>               order      = {});
        void AddPass(const char* name,
                     std::function<void(RenderGraphBuilder&)>   setupFn,
                     std::function<void(RHI::CommandList&)>     executeFn,
                     std::function<bool()>                      isActiveFn = {});
        // Lambda pass @p name records in a secondary view too (PassLifecycle::kViewSafe): what it reads
        // and writes is the bound view's. A class pass says so in its HandledEvents.
        void MarkViewSafe(const char* name);

        // ---- Graph-scope texture declaration ----
        // The handle is stable across Compile() calls; safe to capture in lambdas.
        // @p residency: see TextureResidency. Re-declaring an existing name returns
        // its handle and ignores the new desc and residency.
        RGTextureHandle CreateTexture(const char* name, const RGTextureDesc& desc,
                                      TextureResidency residency = TextureResidency::Always);

        // Backs a TextureResidency::OnDemand texture. From the first call on it
        // behaves like an Always one: created now if the graph is compiled (at the
        // compiled size - a graph has ONE size, and a pending resize recompiles
        // everything), else by the next Compile(), and recreated by every Compile()
        // after that. Later calls, and any call on an Always texture, are no-ops, so
        // a producer may call it every frame it is wanted.
        // The demand is STICKY: nothing withdraws it short of Destroy(), and there is
        // deliberately no release. A producer switched off keeps its whole working
        // set (RTDIPass / RTGIPass on a render-path switch): the restir_switch_20 pack
        // (tools/verify_restir_switch_memory.py) holds VRAM flat over twenty
        // RT <-> Raster round trips, and freeing the result alone would bring that
        // churn back.
        // THREADING: Compile()'s serial window (it creates a resource and its
        // descriptors), never inside Execute.
        void EnsurePhysicalTexture(RGTextureHandle h, IGraphicsDevice& gfx);

        // ---- Imported textures ----
        // A texture the graph does not own (the HDR scene colour, the device's slot
        // target). Declared once like CreateTexture (a stable handle; passes find it
        // by name in Setup), it is never created, resized, aliased or destroyed by
        // the graph: BindImport hands it the device texture before EVERY Execute,
        // and an Execute without one reports it once and gives the passes no
        // texture. Its tracker starts each Execute from the DEVICE's book
        // (IGraphicsDevice::QueryTextureState) - whatever touched it outside the
        // graph is where the graph starts - and there is no exit state: it stays in
        // the last state a pass declared, and the code after the graph transitions
        // it itself. The clear value travels with the bind: a Load::Clear of the
        // import clears to it. Re-declaring a name returns its handle.
        // @p spec (ImportSpec): a source the graph reads instead of a BindImport, the
        // entry the DAG assumes and who transitions it - for an import whose producer
        // is outside the graph (the CSM); a pass in the graph hands its own over in
        // RenderPass::DeclareImports.
        // TODO(RenderGraph_AutoTopology_NodeView_Plan M7): bring the code after the graph into it.
        RGTextureHandle ImportTexture(const char* name, const RGTextureDesc& desc,
                                      const ImportSpec& spec = {});
        // Serial window, before Execute. @p clear may be null (black).
        void BindImport(RGTextureHandle h, const RHI::Texture& texture, const float clear[4]);
        // The clear value of this frame's bind; null when @p h is not an import.
        const float* ImportClearValue(RGTextureHandle h) const;
        // -rgimportoff: the owners declare no import - the A/B reference arm.
        // Resolved once from the command line.
        static bool ImportsEnabled();
        // -rgplaneoff: a depth-stencil texture is tracked whole; DepthStencil,
        // ReadSRV(h, Stencil) and CopySrc / CopyDst mean WriteDepthStencil, ReadSRV
        // and a whole copy, and the per-plane passes fall back to their whole-texture
        // declarations and barriers: the same-binary reference arm. Resolved once
        // from the command line.
        static bool PlanesEnabled();

        // Calls Setup() on all passes, creates physical GPU textures via gfx,
        // then calls Init() on each pass not yet initialised (once per pass) and
        // OnCompile() on all passes.
        void Compile(IGraphicsDevice& gfx);

        // Force a recompile on the next Execute() call (recreates all virtual textures).
        // Call when the window is resized so graph-managed textures match the new dimensions.
        void MarkDirty() { m_compiled = false; }

        // ---- Teardown ----
        // Unlike MarkDirty (which keeps every pass object and graph-texture
        // DECLARATION and rebuilds the physical textures on the next Compile),
        // Destroy gives the whole graph back and leaves it EMPTY. It exists for a
        // SECOND graph instance (a preview GraphSet) that lives shorter than the
        // process; the main graph is never destroyed and must not be.
        // It releases:
        //   * every physical texture but the imports, through
        //     IGraphicsDevice::DestroyTexture (the frame-stamped DEFERRED bucket: the
        //     resource and its descriptors are freed only once the frames that
        //     referenced them have retired);
        //   * every pass's own GPU resources, via RenderPass::Shutdown (a pass that
        //     BORROWS another instance's ShaderLibrary must not destroy it - see
        //     GBufferPass::Shutdown);
        //   * the pass objects, the graph-texture declarations, the name table, the
        //     view (back to showing everything), the external-wait map and every
        //     per-frame binding.
        // It does NOT touch what the graph never owned: borrowed libraries, draw
        // lists, bound constant buffers.
        // THREADING: serial window only, and NOT inside any graph's Execute (Phase B
        // records on task-system workers): destroying descriptors beside a graph
        // worker is the hazard Compile is kept out of Renderer::Render for.
        DestroyStats Destroy(IGraphicsDevice& gfx);

        // ---- Secondary-instance configuration ----
        // For a SECOND graph instance (a preview graph); the defaults leave the main
        // graph unaffected.

        // Pins the render dimensions instead of reading them off the device; 0/0
        // (the default) asks the device. A preview graph draws into its own target,
        // whose size has nothing to do with the device's HDR target, so it pins the
        // pair here. Physical textures are sized at Compile() time, so changing this
        // on a compiled graph requires MarkDirty(). Serial phase only.
        void SetFixedDimensions(uint32_t w, uint32_t h)
        {
            m_fixedWidth  = w;
            m_fixedHeight = h;
        }

        // Allocates one command list per pass (IGraphicsDevice::BeginCommandList),
        // emits per-pass barriers, records draws, and chains execution dependencies
        // so passes run in declaration order on the GPU.
        // Returns the command list the last ACTIVE pass finished recording on - its
        // Execute return value, not necessarily the CL the graph allocated for it
        // (see RenderPass::Execute) - for callers that chain further dependencies
        // onto it. Returns an invalid CommandList when the graph has no passes or
        // none ran (not shown, or IsActive() false): test IsValid() before chaining.
        RHI::CommandList Execute(IGraphicsDevice& gfx, const float clearColor[4]);
        Sys::OwnerTask ExecuteAsync(IGraphicsDevice& gfx, const float* clearColor, RHI::CommandList& result);

        // Registers a cross-queue dependency for one pass: when Execute() opens that
        // pass's CL it adds @p depCL via AddCommandListDependency. This lands an
        // async-compute producer's wait at exactly the consumer (LightingPass waits
        // for the DDGI compute-queue CL) instead of stalling the whole graph at its
        // first pass, which would lose the async-compute overlap with GBuffer /
        // Shadow / SkyIBL.
        // @p passName matches RenderPass::InstanceName(); a name no registered pass
        // has is reported (ENGINE_CHECK_ONCE) here instead of vanishing. The wait map
        // is one-shot - cleared at the END of each Execute(), on its nothing-active
        // early-out and by Destroy() - so callers re-register every frame (as with
        // SetDrawList / BindBuffer).
        void SetExternalWait(const char* passName, RHI::CommandList depCL);
        // The list the next Execute's first list runs after (one-shot: taken at that Execute's start).
        // A secondary view's graph runs after the main view's frame, whose results it reads (the
        // per-object and local-light shadow maps, the probes) and whose shared arrays it rewrites.
        void SetExecuteAfter(RHI::CommandList after) { m_executeAfter = after; }
        // Owner-only publication after setup and submission have stopped reading
        // live game state. The callback must not pump this executor recursively.
        void SetRecordingReadyCallback(std::function<void()> callback) { m_recordingReady = std::move(callback); }

        // Hot-reload entry point: calls RenderPass::ReloadShaders on every
        // registered pass. The caller ensures the GPU is idle (no in-flight commands
        // on the old PSOs): the App calls it between EndFrame and the next
        // BeginFrame, after the device has waited on the previous frame.
        void ReloadShaders(IGraphicsDevice& gfx);

        // True when @p pass is one of this graph's passes (for the pass census).
        bool Contains(const RenderPass* pass) const;

        // Register a per-frame constant buffer; must be called before Execute.
        void BindConstantBuffer(const char* name, const RHI::GPUBuffer& buffer);

        // Register a GPU buffer for root SRV binding (InstanceBuffer, MeshDescriptors etc).
        // Must be called before Execute. Accessed in passes via RenderContext::GetBuffer.
        void BindBuffer(const char* name, const RHI::GPUBuffer& buffer);

        // Set the bindless g_Buffers[] descriptor table GPU handle for this frame.
        void SetBindlessTableHandle(uint64_t gpuHandle) { m_bindlessTableHandle = gpuHandle; }

        // Supply the frame's sorted draw list so passes can iterate DrawPackets.
        // Must be called before Execute.  Pointer must remain valid during Execute.
        void SetDrawList(const std::vector<DrawPacket>* packets);

        // ---- Setup-phase API (RenderGraphBuilder calls these during Compile) ----
        RGTextureHandle DeclareTexture(const char* name, const RGTextureDesc& desc);
        // A pass's import (ImportDeclarer::Import, from RenderPass::DeclareImports,
        // before any Setup of this Compile). A name already taken is reported and its
        // handle returned.
        RGTextureHandle DeclarePassImport(const char* name, const RGTextureDesc& desc,
                                          const ImportSpec& spec);
        RGTextureHandle LookupTexture(const char* name) const;
        void            RecordWrite(RGTextureHandle h, bool isDepth);
        void            RecordRead(RGTextureHandle h, bool history = false);
        void            RecordUAV(RGTextureHandle h);
        // A render target with its load op (WriteRenderTarget is Load), and a SCOPED
        // read - the pass reads @p h inside its own list and puts it back, e.g.
        // CopyHdrSceneToScoped's RT -> COPY_SOURCE -> RT: Phase A brings it into
        // @p state, the DAG counts a read, and the graph's tracker stays at @p state
        // across the pass.
        void            RecordRenderTarget(RGTextureHandle h, Load load);
        void            RecordScopedRead(RGTextureHandle h, RHI::ResourceState state);
        // "I touch this graph texture and I manage its barriers MYSELF." Suppresses
        // the undeclared-access warning without asking the graph to transition it -
        // for what the declaration model cannot express, chiefly depth bound as DSV
        // and SRV at once (ReadSRV + WriteDepthStencil would ask for two
        // contradictory states). Keeping the exception EXPLICIT keeps the validator
        // baseline at zero, so a NEW violation stands out.
        // @p entryState (optional) is the state the pass's own barrier pair starts
        // FROM. A pass cannot know the texture's state when it begins - that depends
        // on which earlier passes were ACTIVE this frame (after a ReadSRV the graph
        // leaves depth in DEPTH_READ_SRV) - so the graph transitions INTO entryState
        // in Phase A: the pass's hardcoded before-state is correct by construction
        // and the graph's tracker stays in step. UNDEFINED = no transition.
        void            RecordManualBarrier(RGTextureHandle h,
                                            RHI::ResourceState entryState =
                                                RHI::ResourceState::UNDEFINED);
        void            SetBuiltinTarget(BuiltinTexture t);
        // Called by RenderGraphBuilder::DepthStencil, ReadSRV(h, Plane::Stencil) and
        // CopySrc / CopyDst.
        void            RecordDepthStencil(RGTextureHandle h, Depth depth, Stencil stencil, uint8_t stencilMask);
        void            RecordStencilRead(RGTextureHandle h);
        void            RecordCopy(RGTextureHandle h, Plane plane, bool dst);

        // Expose the physical GPU texture for a graph-managed handle.
        // Returns nullptr if the handle is invalid or the graph has not been compiled.
        // Use this to obtain SRV/UAV GPU handles for passes running outside the graph.
        // A caller outside this graph's passes must have declared the texture with
        // DeclareOutsideUse; one that has not is logged once ([RG_ALIAS] audit).
        const RHI::Texture* GetPhysicalTexture(RGTextureHandle h) const;
        // The same texture for a viewer that may show any of them (the debug target view, its
        // producers, a pass capture): not an outside use, and never audited.
        const RHI::Texture* InspectPhysicalTexture(RGTextureHandle h) const;

        // ---- Views (Graphics/ViewSlot.h) ----
        // View @p slot's physical textures become the graph's - every Execute, GetPhysicalTexture,
        // MovePlanes and state query - until the next BindViewSet; slot 0's are the ones Compile made
        // (the main view's). A secondary view's are made here, at the device's render size (its HDR
        // slot's) or the pinned one, the first time and whenever that size or the compile changed; their
        // states are their own. Imports are bound per Execute as ever. Serial, outside Execute.
        void     BindViewSet(IGraphicsDevice& gfx, uint32_t slot);
        uint32_t BoundViewSet() const { return m_boundSet; }
        // Gives view @p slot's textures back (deferred destruction). Not while it is bound.
        void     ReleaseViewSet(IGraphicsDevice& gfx, uint32_t slot);
        // The secondary views' graph textures that live only inside their segment share two alias heaps
        // (render targets and depth; the rest): views record one after another, and a view's graph waits on
        // the previous view's last list. The first pass of a view's Execute activates them (an aliasing
        // barrier, then Discard for a render target or depth); ParkViewSet leaves its render targets and depth
        // in their write states after its last list, so the next activation needs no transition over bytes
        // another view wrote. -rgaliasoff: committed, as before.
        // ParkViewSet returns the parking list, after @p after (invalid when there is nothing to park).
        RHI::CommandList ParkViewSet(IGraphicsDevice& gfx, uint32_t slot, RHI::CommandList after);
        struct ViewSetBytes
        {
            uint64_t committed   = 0;   // its own graph textures' bytes
            uint64_t placed      = 0;   // the bytes of the ones in the alias heaps (shared with the other views)
            uint32_t placedCount = 0;
        };
        ViewSetBytes GetViewSetBytes(IGraphicsDevice& gfx, uint32_t slot) const;
        // The views' alias heaps' size: the graph textures' two (render targets and depth + the rest) and the
        // scratch heap the passes' per-view transients share.
        uint64_t ViewAliasHeapBytes() const;
        // The heap secondary views' pass transients (RT reflections, AO) are placed in; made on first use.
        TransientAliasArena& ViewScratchArena();
        // Gives the views' heaps back once no secondary view exists: every view's textures must be released
        // first (their placed textures keep a heap alive until D3D12 destroys them).
        void ReleaseViewHeaps();

        // ---- Memory aliasing ----
        // Why a graph texture keeps memory of its own. None: it could share a heap with the
        // textures whose lifetimes do not overlap its own.
        enum class AliasPin : uint8_t
        {
            None,
            Imported,        // the device's, bound per Execute
            OnDemand,        // backed only when its producer asks
            OutsideUse,      // read or written outside the graph's passes (DeclareOutsideUse)
            FirstUseLoads,   // its first user keeps what is there (a Load render target, a DSV, a UAV)
            CrossFrame,      // its first user reads it: last frame's content
            Unused,          // no pass declares it
        };
        static const char* AliasPinName(AliasPin pin);
        struct AliasRow
        {
            std::string name;
            AliasPin    pin   = AliasPin::None;
            int32_t     first = -1;    // the execution-order index of the first pass that declares it
            int32_t     last  = -1;    // ... and of the last
            uint64_t    bytes = 0;     // a placed allocation's size (0: the device could not say)
            bool        rtds  = false; // a render target or depth texture (the RT / DS heap)
            std::string outsideBy;     // who declared an outside use
        };
        // Code outside this graph's passes (a Renderer phase, the SSR chain, TAA) reads or writes
        // @p h, so it keeps memory of its own. Before Compile, where the handle is handed out; the
        // declaration lasts as long as the texture.
        void DeclareOutsideUse(RGTextureHandle h, const char* who);
        // The last Compile's plan, a row per graph texture. Any thread.
        std::vector<AliasRow> AliasPlan() const;

        // ---- State export / import for out-of-graph passes ----
        // After Execute(), out-of-graph passes (HiZ, post-processing) query the state
        // a texture was left in and set it after they transition it, so no graph
        // boundary hardcodes a state. For a depth-stencil texture the two-argument
        // forms are its depth plane's state / every plane; the Plane forms one plane.
        RHI::ResourceState GetTextureState(RGTextureHandle h) const;
        void               SetTextureState(RGTextureHandle h, RHI::ResourceState state);
        RHI::ResourceState GetTextureState(RGTextureHandle h, Plane plane) const;
        void               SetTextureState(RGTextureHandle h, RHI::ResourceState state, Plane plane);
        // Moves @p h's planes to @p depthTarget / @p stencilTarget on @p cl for a
        // user outside the graph (Hi-Z, the SSR chain, DDGIProbeDebug, the debug
        // views, the pre-compute and restore lists), from and into this book: a
        // barrier per plane only where it differs (planes in one state going to one
        // target is one barrier on every plane); UNDEFINED leaves that plane. Under
        // -rgplaneoff, or for a texture with one plane, the whole texture moves to
        // @p depthTarget. An owner-tracked import is its owner's to move.
        // TODO(RenderGraph_AutoTopology_NodeView_Plan M7): bring those users into the graph.
        void               MovePlanes(IGraphicsDevice& gfx, RHI::CommandList cl, RGTextureHandle h,
                                      RHI::ResourceState depthTarget, RHI::ResourceState stencilTarget);

        // ---- Introspection (the -dump_catalog harness dump) ----
        // One entry per pass in declaration order, which IS execution order (an
        // inactive pass is skipped, never reordered). The textures are what the pass
        // declared in Setup, by graph name; `colorTarget` is the implicit builtin
        // target, not in those lists. `ran` = the LAST Execute recorded it
        // (Shown() && IsActive()), false before the first; `shown` = the graph's
        // view shows it now. Serial windows only (Compile's discipline): never while
        // Execute runs.
        std::vector<PassInfo> DescribePasses() const;

        std::vector<TextureInfo> DescribeTextures() const;

        // The texture named @p name (as CreateTexture / DescribeTextures name
        // it; case-sensitive), invalid when there is none. Read-only, unlike
        // CreateTexture's re-declaration. Serial windows only.
        RGTextureHandle FindTexture(const std::string& name) const;
        // Did a pass the LAST Execute recorded write @p h (render target,
        // depth, UAV or self-barriered)? False before the first Execute. What
        // the debug target view (Renderer/DebugTargets.h) calls "written this
        // frame". Serial windows only.
        bool WrittenByLastExecute(RGTextureHandle h) const;

        // ---- Executed-frame snapshots (RenderGraph/GraphSnapshot.h) ----
        // Publishes a GraphSnapshot of the frame to @p board whenever it holds an
        // unanswered Request(), built inside Execute after Phase C from what that
        // Execute did. nullptr (the default) = never: the preview graphs publish
        // nowhere. A frame nobody asked about costs one relaxed atomic load. Serial
        // window, before Execute.
        void SetSnapshotBoard(SnapshotBoard* board) { m_snapshotBoard = board; }
        // The frame number the next snapshot carries. The graph counts no
        // frames of its own; the owner stamps its frame index here each frame.
        void SetSnapshotFrame(uint64_t frame) { m_snapshotFrame = frame; }
        // The snapshot of the last frame the device journaled (a graph with a board
        // snapshots every such frame), handed over once for the frame capture join
        // (RenderGraph/FrameCapture.h). Null when that frame did not execute this
        // graph. Serial window, after Execute.
        std::shared_ptr<const GraphSnapshot> TakeFrameCaptureSnapshot()
        { return std::exchange(m_frameCaptureSnapshot, nullptr); }

        // ---- The view this graph draws (RenderGraph/SceneView.h) ----
        // The owner sets it where it knows what the view shows (the Renderer: once
        // the view mode and the path tracer's camera primary are known; the preview:
        // before each Execute). Every pass's Shown() is set from its
        // RequiredShowFlags at once; the next Execute skips the passes not shown (no
        // command list, no barrier, no Execute, whatever IsActive or -passactiveoff
        // say) and hands RenderContext::GetView() this view. A graph never given one
        // shows everything; a pass added later joins under the current view
        // (AddPass); Reset goes back to showing everything.
        // Four disciplines hold for a hidden pass:
        //   1. A hidden pass that is the target of SetExternalWait has its wait
        //      re-homed onto the next pass that runs, and dropped (one warning per
        //      session) when none does: name a pass that will run (the preview asks
        //      its shadow pass's Shown()). No main-graph wait targets a pass that
        //      declares a flag.
        //   2. What a view hides must be PRODUCER-COMPLETE: barrier tracking is
        //      state-based, so hiding a texture's producer lets a still-shown
        //      consumer read last frame's content with no warning. The flags are on
        //      passes that write only their own targets or draw over targets this
        //      frame already filled (the sky, the fogs and the clouds over the scene
        //      colour, the water over the GBuffer's); GBufferPass (the HdrSceneColor
        //      clear's declarer) and LightingPass declare none.
        //   3. Nothing can hide every pass (2), and a view with nothing to draw does
        //      not call Execute at all (the preview's suspended split cell); Execute
        //      still returns an invalid list when no pass ran.
        //   4. The view is STICKY, not per-frame state: Execute never resets it (the
        //      nothing-active early-out included). Serial window, not while Execute
        //      runs - the discipline IsActive() has.
        void             SetView(const SceneView& view);
        const SceneView& View() const { return m_view; }

        // ---- A texture's content right after one pass ----
        // The next Execute copies the texture named @p texture into @p dst on a
        // command list of its own, right after the pass whose instance name is
        // @p afterPass (empty = the last pass that runs): recorded in Phase A before
        // the next active pass, or after the graph's tail when none follows - in the
        // order the GPU runs it, so the graph's book and the device's move together
        // (MovePlanes to COPY_SRC and back) and the tripwires stay quiet. That
        // Execute resolves the names, after its Compile. @p dst is the caller's, with
        // the texture's desc; @p dstState is its state, left SHADER_RESOURCE. A
        // snapshot frame records nothing (its journal would carry the copy): the
        // result says deferred. One arm, one Execute; an Execute that runs no pass
        // leaves it for the next. Only the main graph is armed. Serial window, before
        // Execute.
        void ArmTextureCopy(std::string texture, std::string afterPass, RHI::Texture* dst,
                            RHI::ResourceState* dstState);

        // The last Execute's result, once (the render half, after Execute).
        TextureCopyResult TakeTextureCopyResult() { return std::exchange(m_copyResult, TextureCopyResult{}); }
        // The pass whose RenderPass::InstanceName is @p name; ~0u when none.
        // PassName: the instance name of pass @p index, nullptr out of range.
        // Serial windows.
        uint32_t    FindPass(const std::string& name) const;
        const char* PassName(uint32_t index) const;

    private:
        // Graph-scope texture spec (set by CreateTexture, survives Compile resets).
        // `demanded` is EnsurePhysicalTexture's sticky flag (OnDemand only); it
        // lives here, not in VirtualTexture, because every Compile() rebuilds
        // m_textures.
        struct GraphTexture
        {
            std::string      name;
            RGTextureDesc    desc;
            TextureResidency residency = TextureResidency::Always;
            bool             demanded  = false;
            // ImportTexture: BindImport's texture and clear value, taken by the next
            // Execute (boundThisFrame goes false there).
            bool             imported       = false;
            bool             boundThisFrame = false;
            RHI::Texture     bound;
            float            clear[4] = { 0.f, 0.f, 0.f, 0.f };
            ImportSpec       spec;   // source / entry / tracking
            std::string      outsideBy;   // DeclareOutsideUse's callers, comma separated
        };

        // Physical backing for a virtual texture declared by a pass.
        struct VirtualTexture
        {
            std::string        name;
            RGTextureDesc      desc;
            RHI::Texture       texture;
            RHI::ResourceState currentState = RHI::ResourceState::UNDEFINED;
            // A depth-stencil format (planar): currentState is its depth plane's,
            // stencilState its stencil plane's.
            RHI::ResourceState stencilState = RHI::ResourceState::UNDEFINED;
            bool planar = false;
            bool imported = false;   // the device's texture, bound per Execute - never created or destroyed here
            // ImportSpec: an import's source (read at each Execute's start), its entry
            // and its tracking.
            const RHI::Texture* source   = nullptr;
            ImportEntry         entry    = ImportEntry::Stale;
            ImportTracking      tracking = ImportTracking::Graph;

            bool hasRTV = false;
            bool hasDSV = false;
            bool hasSRV = false;
            bool hasUAV = false;
            bool outsideDeclared = false;   // a DeclareOutsideUse names it (the audit's test)

            bool OwnerTracked() const { return imported && tracking == ImportTracking::Owner; }
        };

        struct PassNode
        {
            struct DsvDecl
            {
                RGTextureHandle h;
                Depth           depth       = Depth::None;
                Stencil         stencil     = Stencil::None;
                uint8_t         stencilMask = 0xFF;
            };

            std::unique_ptr<RenderPass>  pass;
            BuiltinTexture               colorTarget = BuiltinTexture::None;
            Stage                        stage = Stage::Unstaged;   // AddPass's tag; Setup never changes it
            // Which AddPass it was in this tenancy (the first sort's input) and its
            // order constraints (RenderGraph/Order.h).
            uint32_t                     registered = 0;
            std::vector<OrderRule>       order;
            std::vector<RGTextureHandle> rtHandles;
            std::vector<RGTextureHandle> dsHandles;
            std::vector<RGTextureHandle> srvHandles;
            std::vector<RGTextureHandle> historyHandles;   // the srvHandles declared ReadHistory
            std::vector<RGTextureHandle> uavHandles;
            std::vector<RGTextureHandle> manualHandles;  // self-barriered, see RecordManualBarrier
            // Parallel to manualHandles. UNDEFINED = "do not transition"; any
            // other value makes Phase A bring the texture into that state
            // before the pass records, so the pass's own hardcoded
            // before-state is a declared contract rather than a guess.
            std::vector<RHI::ResourceState> manualEntryStates;
            // Parallel to rtHandles: each render target's load op.
            std::vector<Load>               rtLoads;
            // ReadScoped: read inside the pass's own list at a state it is put back in;
            // parallel vectors.
            std::vector<RGTextureHandle>    scopedHandles;
            std::vector<RHI::ResourceState> scopedStates;
            // The HDR import the colorTarget shim declared (invalid = none, e.g.
            // -rgimportoff). Phase A then emits the graph's barriers BEFORE the builtin
            // bind, so the import's entry transition joins this pass's batch, and the
            // bind clears to the import's value.
            RGTextureHandle              hdrShim;

            // The DepthStencil declarations; ReadSRV of a stencil plane; the copies
            // (texture, plane); and Phase A's DSV view per DepthStencil declaration
            // (texture id, view), which RenderContext::DepthView hands the pass while it
            // records.
            std::vector<DsvDecl>                             dsvDecls;
            std::vector<RGTextureHandle>                     stencilSrvHandles;
            std::vector<std::pair<RGTextureHandle, Plane>>   copySrc, copyDst;
            std::vector<std::pair<uint32_t, RHI::DepthView>> depthViews;
            bool                         initialized = false;
        };

        struct PassRecord
        {
            RHI::CommandList cl{};
            uint32_t profilerRegion = 0;
            RHI::CommandList returnedCL{};
        };

        struct TextureCopyArm
        {
            bool                armed    = false;
            std::string         texture, afterPass;
            RGTextureHandle     handle{};          // resolved by Execute
            RHI::Texture*       dst      = nullptr;
            RHI::ResourceState* dstState = nullptr;
        };

        RHI::CommandList  RecordTextureCopy(IGraphicsDevice& gfx, RHI::CommandList after, const TextureCopyArm& arm);

        void SortPasses();
        void CheckStageOrder();

        void ApplySchedule();
        void PermutePasses(const std::vector<uint32_t>& perm);
        // Checks the stencil owners (RenderGraph/StencilBits.h) each Compile.
        void CheckStencilOwners();

        void DeriveFrameDag();

        void CreatePhysicalTextures(IGraphicsDevice& gfx);
        bool CreatePhysicalTexture(VirtualTexture& vt, IGraphicsDevice& gfx);
        // The physical texture @p vt is created as (its size from the last CreatePhysicalTextures);
        // @p layout receives the state it is created in.
        RHI::TextureDesc PhysicalDesc(const VirtualTexture& vt, RHI::ResourceState& layout) const;

        // ---- Memory aliasing ----
        // Compile, after the textures are created: each graph texture's lifetime and pin
        // (m_aliasPlan) and, on the main graph, the [RG_ALIAS] census line.
        void BuildAliasPlan(IGraphicsDevice& gfx);
        // GetPhysicalTexture / MovePlanes on texture @p id from code outside this graph's passes
        // without a DeclareOutsideUse, or from a pass of it that did not declare the texture:
        // logged once per texture ([RG_ALIAS] audit).
        void AuditPhysicalAccess(uint32_t id) const;

        // ---- Views ----
        // A view's physical textures while another view's are the graph's: the bound set lives in
        // m_textures and m_physicalWidth / Height, the others in m_viewSets (slot 0's while a
        // secondary one is bound). compile = the m_compileCount they were made under.
        struct ViewTexture
        {
            RHI::Texture       texture;
            RHI::ResourceState state        = RHI::ResourceState::UNDEFINED;
            RHI::ResourceState stencilState = RHI::ResourceState::UNDEFINED;
            bool               placed       = false;   // in a view alias heap (PlaceBoundSet)
        };
        struct ViewSet
        {
            std::vector<ViewTexture> textures;
            uint32_t                 width   = 0;
            uint32_t                 height  = 0;
            uint64_t                 compile = 0;
            bool                     placedTried    = false;   // PlaceBoundSet ran for these textures
            uint32_t                 placedEpoch[2] = {};      // the heaps' epochs it placed them on
            uint32_t                 placedDemand   = 0;       // the on-demand textures it placed (DemandedCount)
        };
        // The bound set's physical textures and states into / out of @p set (BindViewSet). A
        // secondary set has no entry of its own for a shared texture (ViewShared): it loads slot 0's,
        // and its states go back there.
        // How many on-demand textures have been asked for: a set placed before one more was is placed again.
        uint32_t DemandedCount() const;
        void StashViewSet(ViewSet& set);
        void LoadViewSet(const ViewSet& set);
        // Texture @p id is the main view's in every view: no view-safe pass writes it (m_viewShared).
        bool ViewShared(size_t id) const;
        // Backs every texture of the bound set that should be (not imported; Always, or OnDemand and
        // demanded) and is not.
        void BackBoundSet(IGraphicsDevice& gfx);
        // A secondary bound set's textures that live only inside its segment, placed in the view alias heaps
        // (once per set; again when a heap grew since): AliasPlan's candidates, outside uses and first-use
        // loads - not the shared, imported, OnDemand or cross-frame ones.
        void PlaceBoundSet(IGraphicsDevice& gfx);
        // The first active pass of a secondary view's Execute: its placed textures made active.
        void ActivateBoundSet(IGraphicsDevice& gfx, RHI::CommandList cmd);
        // @p capture (a snapshot frame only) receives each transition as it is
        // batched, @p uavCapture each texture that got a UAV barrier; nullptr on
        // every other frame. @p passIndex is the node's index.
        void EmitBarriersBeforePass(IGraphicsDevice& gfx,
                                    RHI::CommandList  cmd,
                                    PassNode&         node,
                                    uint32_t          passIndex,
                                    std::vector<GraphSnapshot::Barrier>* capture = nullptr,
                                    std::vector<uint32_t>* uavCapture = nullptr);
        // The snapshot of the Execute that just finished (or stopped at its
        // nothing-active early-out, @p recorded false: no CL, no record times, no
        // waits). Reads the per-frame scratch Execute leaves behind; @p gfx names the
        // imports' resources.
        std::shared_ptr<GraphSnapshot> BuildSnapshot(
            const IGraphicsDevice& gfx, bool recorded,
            const std::vector<std::vector<GraphSnapshot::Barrier>>& barriers,
            const std::vector<std::vector<uint32_t>>& uavBarriers,
            const std::vector<uint32_t>& initialStates,
            const std::vector<uint32_t>& initialStencilStates) const;

        bool                                                    m_compiled = false;
        const std::vector<DrawPacket>*                          m_drawList = nullptr;
        uint64_t                                                m_bindlessTableHandle = 0;
        // See SetFixedDimensions. 0/0 = take the dimensions from the device.
        // Sticky, NOT per-frame state: Execute never resets it.
        uint32_t                                                m_fixedWidth  = 0;
        uint32_t                                                m_fixedHeight = 0;
        std::vector<GraphTexture>                               m_graphTextures;
        std::vector<PassNode>                                   m_passes;
        std::vector<double> m_cpuPassMs;
        uint32_t m_cpuProfileFrames = 0;
        std::vector<VirtualTexture>                             m_textures;
        std::unordered_map<std::string, uint32_t>               m_nameToIndex;
        // Named CB / buffer bindings. BindConstantBuffer / BindBuffer resolve the
        // name to an RGSlot ONCE at registration; Execute copies raw pointers into
        // the RenderContext arrays, so the frame loop hashes no strings. Names
        // without an RGSlot fall into the string maps (cold path, so editor / debug
        // code can bind ad-hoc names without touching the enum).
        std::array<const RHI::GPUBuffer*, size_t(RGSlot::Count)> m_cbSlotBindings{};
        std::array<const RHI::GPUBuffer*, size_t(RGSlot::Count)> m_bufferSlotBindings{};
        std::unordered_map<std::string, const RHI::GPUBuffer*>  m_cbFallback;
        std::unordered_map<std::string, const RHI::GPUBuffer*>  m_bufferFallback;
        // Pass name -> cross-queue dependency CL, filled by SetExternalWait,
        // consumed and cleared by Execute. RHI::CommandList is a trivially copyable
        // handle.
        std::unordered_map<std::string, RHI::CommandList>       m_externalWaits;
        RHI::CommandList                                         m_executeAfter{};   // SetExecuteAfter's
        // One-shot log guards for the external-wait / inactive-pass interaction
        // (see Execute). Without them the message would repeat every frame for
        // as long as the pass stays inactive.
        bool m_extWaitRehomeLogged  = false;
        bool m_extWaitDroppedLogged = false;

        // Owned by this graph, reused only after its synchronous Execute joins.
        // Separate graph instances (main/preview) never share recording scratch.
        RenderContext m_executeContext;
        std::vector<PassRecord> m_passRecords;
        std::vector<std::pair<uint32_t, uint32_t>> m_extraRecordTasks;
        std::function<void()> m_recordingReady;
        std::vector<double> m_recordCostMs; // scheduling hints only; never dependencies
        std::vector<double> m_recordSamplesMs;
        std::vector<uint32_t> m_recordOrder;
        uint64_t m_recordScheduleEpoch = 0;
        std::vector<uint8_t> m_activePasses;
        // [RG_ACTIVE] rate limiter. Per INSTANCE, so the main graph and each
        // preview GraphSet each print on their own cadence instead of sharing
        // one static counter and printing one line in N.
        uint64_t m_activeLogFrames = 0;
        // -passactiveverify rate limiter, same per-INSTANCE reasoning.
        uint64_t m_activeVerifyFrames = 0;
        std::vector<std::vector<RHI::CommandList>> m_executeWaits;
        std::vector<RHI::GPUBarrier> m_barrierScratch;
        bool m_executing = false; // Execute on one graph is not reentrant

        PassNode* m_currentSetupNode = nullptr;

        // The size the last CreatePhysicalTextures built at - what
        // EnsurePhysicalTexture sizes a late-backed texture to.
        uint32_t m_physicalWidth  = 0;
        uint32_t m_physicalHeight = 0;

        // See SetView.
        SceneView m_view;

        // See ArmTextureCopy.
        TextureCopyArm    m_copyArm;
        TextureCopyResult m_copyResult;

        // See SetSnapshotBoard.
        SnapshotBoard* m_snapshotBoard = nullptr;

        // ---- Views (BindViewSet) ----
        ViewSet  m_viewSets[RHI::kMaxViews];
        uint32_t m_boundSet     = 0;
        uint64_t m_compileCount = 0;   // Compiles so far: a secondary set made under an older one is remade
        std::string m_viewSkipLogged;  // the last [VIEW] line's passes (a secondary view's skipped ones)
        // Per texture, from Compile: 1 = no pass that records in a secondary view (kViewSafe) writes
        // it, so a secondary view reads the main view's - what the main view's passes made this frame.
        std::vector<uint8_t> m_viewShared;
        // The secondary views' alias heaps: [0] the SRV / UAV textures, [1] render targets and depth.
        std::unique_ptr<TransientAliasArena> m_viewArenas[2];
        std::unique_ptr<TransientAliasArena> m_viewScratch;   // the passes' per-view transients (ViewScratchArena)

        // ---- Memory aliasing (BuildAliasPlan, AuditPhysicalAccess) ----
        mutable std::mutex                      m_aliasMutex;   // m_aliasPlan
        std::vector<AliasRow>                   m_aliasPlan;
        // Per texture, what the audit already logged: bit 0 an undeclared outside use, bit 1 an
        // undeclared use from one of this graph's passes.
        std::unique_ptr<std::atomic<uint8_t>[]> m_auditSeen;
        size_t                                  m_auditSeenCount = 0;
        // See TakeFrameCaptureSnapshot.
        std::shared_ptr<const GraphSnapshot> m_frameCaptureSnapshot;
        uint64_t       m_snapshotFrame = 0;

        // ---- The per-frame DAG (RenderGraph/RenderGraphDag.h) ----
        // m_passAccesses: each pass's declarations in Phase A's transition order,
        // rebuilt by Compile - what the DAG and the snapshot read. The main graph
        // (the one with a snapshot board) derives m_dag before Phase A every frame;
        // m_dagReported holds the findings already logged since the last Compile
        // (check | pass | texture), so each is logged once.
        std::vector<std::vector<GraphSnapshot::PassAccess>> m_passAccesses;
        // Per texture, 1 = an import whose entry is Fresh (written outside, before
        // the graph): the DAG calls no read of it stale.
        std::vector<uint8_t>  m_importFresh;
        // Per texture, 1 = a depth-stencil format (two planes).
        std::vector<uint8_t>  m_planar;
        Dag                   m_dag;
        DagScratch            m_dagScratch;
        double                m_dagUs = 0.0;
        std::vector<uint64_t> m_dagReported;
        // Every graph: the pass whose UAV access left each texture in
        // UNORDERED_ACCESS this Execute (-1 = none, or anything else came after).
        // Reset per Execute; EmitBarriersBeforePass reads and moves it.
        std::vector<int32_t>  m_uavLastPass;
        // -rgtest_swap / -rgtest_uavpair (harness): applied once, at the
        // main graph's first Compile (passes persist across recompiles).
        bool                  m_testSwapApplied = false;
        bool                  m_testDupNameApplied = false;   // -rgtest_dupname
        bool                  m_testUavPairAdded = false;
        bool                  m_testClearAfterLiveAdded = false;   // -rgtest_clearafterlive
        bool                  m_testUnreadAdded = false;           // -rgtest_unread
        bool                  m_testOrderArmsApplied = false;      // -rgtest_ordercycle / -rgtest_registershuffle
        // Compile's first sort ([RG_ORDER]), its stage-order check and the
        // [RG_STAGES] census run once per pass-list change (AddPass clears it), not
        // on every recompile.
        bool                  m_stagesChecked = false;
        uint32_t              m_registered = 0;   // the next AddPass's number

        // The second sort (-rgschedule), once per pass-list change, after Setup: a
        // new order for the passes and their per-pass data.
        bool                  m_scheduleApplied = false;

        // The census CheckStencilOwners last logged (logged again when it changes).
        std::string           m_stencilCensus;

        // ---- Undeclared-access validation ----
        // The graph emits barriers only for textures a pass DECLARED in Setup(). A
        // pass that touches a graph texture without declaring it gets no transition
        // and works only because of the current declaration ORDER - reorder the
        // passes and it breaks silently. That is the most common structural failure
        // in this renderer (Skybox rebinding emissive / SSAO slots, Cloud / Fog
        // reusing the env-cube slot, the depth-as-SRV-and-DSV passes).
        // D3D12 cannot report a resource's real state, so every
        // RenderContext::GetTexture() call is attributed to the pass recording on
        // this thread and checked against that pass's declared handles. Each
        // (pass, handle) pair is reported once. A split pass records on several
        // workers at once (ExecutePrepared plus ExecuteAdditionalRecording, all under
        // its pass index), so OnTextureAccess takes a lock to record an undeclared
        // access in m_undeclaredByPass; a declared access takes none.
// Compiled in ALL configurations on purpose: the workflow runs the RELEASE
// exe, and a _DEBUG-only check would make "0 violations" indistinguishable
// from "never ran". The cost is a null-pointer test plus a short linear scan
// per GetTexture() call. Set it to 0 by hand to remove the check entirely.
#ifndef RG_VALIDATE_ACCESS
    #define RG_VALIDATE_ACCESS 1
#endif
#if RG_VALIDATE_ACCESS

        static thread_local int                s_tlsCurrentPass;    // -1 = not recording
        static thread_local const RenderGraph* s_tlsCurrentGraph;   // the graph s_tlsCurrentPass is of

        void        OnTextureAccess(RGTextureHandle h);
        static void AccessValidatorThunk(void* user, RGTextureHandle h);
        // RenderContext::DepthView: the recording pass's view.
        RHI::DepthView        OnDepthView(RGTextureHandle h) const;
        static RHI::DepthView DepthViewThunk(void* user, RGTextureHandle h);
        void        ReportUndeclaredAccess();

        std::vector<std::vector<uint32_t>> m_undeclaredByPass; // handle ids, deduped
        std::vector<size_t>                m_reportedByPass;   // how many already logged

        bool        m_validationSummaryLogged = false;  // one-shot "is it live" proof
#endif
    };

    // ==== RenderPass: base class for atomic rendering steps ====
    // Its lifecycle hooks (Shutdown, ReloadShaders, the PassSet events) come from
    // RG::PassLifecycle, which the passes outside the graph share.
    class RenderPass : public PassLifecycle
    {
    public:
        // Every instance is in the census (LivePassesSince below) for its
        // whole lifetime; a copy is a new instance.
        RenderPass();
        RenderPass(const RenderPass&);
        RenderPass& operator=(const RenderPass&) { return *this; }
        virtual ~RenderPass();
        virtual const char* GetName() const = 0;
        // This instance's name in its graph: GetName(), or GetName() + "#k" for the
        // k-th pass of that name in one graph. The graph assigns it in Compile, after
        // any SetNameSuffix; until then it is GetName(). It lives in the pass, so the
        // pointer stays valid as long as the pass does (the GPU profiler keeps region
        // names by pointer). Timestamps, markers, command-list names, external waits
        // and the graph's logs all use it.
        const char* InstanceName() const
        {
            return m_instanceName.empty() ? GetName() : m_instanceName.c_str();
        }
        virtual void Setup(RenderGraphBuilder& builder) = 0;
        // The textures this pass hands to other passes, as imports of its own: the
        // pass keeps creating and owning them and names each with
        // ImportDeclarer::Import (a source the graph reads at every Execute, its
        // entry, its tracking). Compile calls it for every pass before ANY Setup, so
        // a reader declared earlier than its producer (Lighting reads the cloud
        // shadow CloudPass writes later) finds the name with
        // RenderGraphBuilder::GetTexture; a graph without that producer (a preview
        // graph) has no such name, and its readers declare nothing. Not called under
        // -rgimportoff.
        virtual void DeclareImports(ImportDeclarer& /*d*/) {}
        virtual void Init(IGraphicsDevice& gfx) = 0;
        // Called serially in RenderGraph::Compile after the physical textures are
        // (re)created. Override to (re)create pass-owned GPU resources that follow
        // the render dimensions, before the parallel Execute.
        virtual void OnCompile(IGraphicsDevice& /*gfx*/) {}

        // Teardown hook: RenderGraph::Destroy calls it before destroying the pass
        // object. Hand back every device resource this instance CREATED (textures,
        // buffers, static descriptor allocations, shader and PSO pool slots) through
        // the device's deferred paths. Three correctness rules:
        //   1. RELEASE ONLY WHAT THIS INSTANCE OWNS. A pass that borrows another
        //      instance's ShaderLibrary (GBufferPass / TransparentPass /
        //      PreviewStudioShadowPass) gates on the OwnsShaderLibrary() test Init
        //      and ClearCaches use: destroying a borrowed library takes the MAIN
        //      viewport's shaders down with it.
        //   2. IT RUNS IN A SERIAL WINDOW, never beside a graph worker.
        //   3. IT MUST BE IDEMPOTENT: reset every handle on the way, so a second call
        //      is a no-op, not a double free.
        // The base is empty: a pass that owns nothing needs no override, and a pass
        // whose resources live for the process says so where it overrides.
        // The main graph is never Destroy()ed: a main-graph pass that must release at
        // shutdown names PassLifecycle::kShutdown in HandledEvents() (SurfaceCacheBake,
        // the ReSTIR / path-trace passes), and the Renderer's PassSet runs it after
        // the GPU drain.
        void Shutdown(IGraphicsDevice& /*gfx*/) override {}

        // Records the pass onto @p cl (cl.GetDevice() / cl.GetContext() give the
        // backend and the per-frame data). Returns the command list it finished
        // recording on, never an invalid one: @p cl, or the last one of its own it
        // opened, which the graph orders the next active pass after.
        virtual RHI::CommandList Execute(RHI::CommandList cl) = 0;

        // Optional flat recording tasks. Prepare runs serially after barriers;
        // it owns allocation/dependency edges. Additional tasks must never join
        // the worker pool. All tasks join before FinishRecording runs serially.
        // Execute remains the standalone, unsplit path for non-graph callers.
        virtual uint32_t PrepareRecording(RHI::CommandList /*cl*/) { return 0; }
        virtual RHI::CommandList ExecutePrepared(RHI::CommandList cl) { return Execute(cl); }
        virtual void ExecuteAdditionalRecording(uint32_t /*task*/) {}
        virtual void FinishRecording() {}

        // Per-frame tick that runs whether or not the pass records. It holds the
        // bookkeeping a pass does BEFORE its first-line early-out (CloudPass clears
        // the three "did I produce anything this frame?" flags that LensFlarePass and
        // the Renderer read AFTER the graph, and drops its temporal history when
        // switched off), so IsActive() can skip the pass without swallowing that work.
        // Contract:
        //   - called from the graph's SERIAL Phase A for EVERY registered pass,
        //     BEFORE the activity gate is queried - including passes the view does
        //     not show (Shown()) and passes about to report themselves inactive;
        //   - it may write pass-owned state, but must NOT touch the command list, the
        //     RenderContext or any GPU resource: no CL exists yet;
        //   - keep the equivalent lines at the top of Execute() as belt-and-braces:
        //     Execute() is also the standalone entry point for non-graph callers,
        //     which never reach this hook.
        virtual void PrepareFrame() {}

        // Activity gate. Returns false to tell the graph "nothing to do this frame",
        // which skips ALL per-pass work: no BeginCommandList (allocator Reset + CL
        // Reset + SetDescriptorHeaps), no render-target bind, no barriers, no GPU
        // timestamp, no debug marker, no Execute. The GPU dependency chain then links
        // the surrounding ACTIVE passes directly, as if the pass were not registered
        // this frame.
        // Contract - read before overriding:
        //   - queried in RenderGraph::Execute's SERIAL Phase A, BEFORE Execute runs:
        //     depend only on state already settled then (Renderer-pushed per-frame
        //     members, PSO / resource validity), never on what Execute computes or
        //     on the RenderContext;
        //   - MUST be side-effect free: a query, not a tick;
        //   - should MIRROR the pass's own first-line early-out, which stays as
        //     belt-and-braces. Per-frame work BEFORE that early-out (resetting a
        //     "did I render?" flag) moves to PrepareFrame(), or the pass stays
        //     always-active - the skip would swallow it;
        //   - open a new override with `!RG::PassActivityGatesEnabled() ||` so
        //     -passactiveoff restores "always true" for A/B (not for a mutually
        //     exclusive pair; see PassActivityGatesEnabled).
        virtual bool IsActive() const { return true; }

        // ---- What the view shows (RenderGraph/SceneView.h) ----
        // The ShowFlag bits this pass draws (RG::ShowFlag as uint32_t; 0 = it draws
        // in every view). Shown(): the graph's view has them all - set by
        // RenderGraph::SetView at once (the forward fog asks before the graph runs),
        // true for a pass no view was set for (a standalone caller). The graph's
        // activity gate skips a pass that is not shown, IsActive or not; a pass that
        // reads it elsewhere (its early-out, PrepareFrame, a forward contribution)
        // reads Shown(), never a flag of its own.
        virtual uint32_t RequiredShowFlags() const { return 0; }
        bool             Shown() const { return m_shown; }

        // Hot-reload accessors. Override these to expose owned ShaderLibrary /
        // PSOCache instances; the default ReloadShaders below clears them uniformly.
        // The low-cost path: a 2-line override instead of a full ReloadShaders body.
        virtual ShaderLibrary*           GetReloadableShaderLibrary() { return nullptr; }
        virtual std::vector<PSOCache*>   GetReloadablePSOCaches()     { return {}; }

        // Hot-reload hook; RenderGraph::ReloadShaders calls it when an .hlsl / .hlsli
        // file changes on disk.
        // Default: clears GetReloadableShaderLibrary / GetReloadablePSOCaches. The
        // next frame's draws hit empty caches -> ShaderLibrary reloads what the edit
        // changed, PSOCache rebuilds the PSO (a brief first-draw stutter, correct
        // with no per-pass code beyond the accessors). When the library reports
        // nothing changed (ClearCaches returns false) the PSO caches are left alone.
        // A pass that builds a PSO into a MEMBER (not through a PSOCache it fetches
        // from every frame) must override this and rebuild it: the default would
        // leave that PSO on the old bytecode while the reload's receipt counts the
        // library as reloaded.
        // Override also when a pass needs more (eager warm-up of specific
        // permutations, refreshing reflection pointers, recreating descriptor
        // heaps); GBufferPass / LightingPass / ShadowPass do, to warm up their
        // default-permutation PSOs.
        void ReloadShaders(IGraphicsDevice& /*gfx*/) override;
    private:
        friend class RenderGraph;
        std::string m_instanceName;   // see InstanceName(); assigned by RenderGraph::Compile
        bool        m_shown = true;   // see Shown(); assigned by RenderGraph::SetView
    };

    // ==== The RenderPass census ====
    // Every live RenderPass, numbered in construction order. Renderer::Compile
    // takes a mark before it builds anything and, at its end, requires each pass
    // constructed since to be in the main graph or in its lifecycle lists
    // (Renderer/RenderLifecycle.h). Thread-safe; the returned pointers stay
    // valid only while nothing destroys those passes (Compile's serial window
    // destroys none).
    uint64_t PassCensusMark();
    std::vector<RenderPass*> LivePassesSince(uint64_t mark);   // construction order

    // ==== RenderGraphBuilder: passed to RenderPass::Setup ====
    class RenderGraphBuilder
    {
    public:
        explicit RenderGraphBuilder(RenderGraph& graph) : m_graph(graph) {}

        RGTextureHandle DeclareTexture(const char* name, const RGTextureDesc& desc)
        {
            return m_graph.DeclareTexture(name, desc);
        }
        RGTextureHandle GetTexture(const char* name) const
        {
            return m_graph.LookupTexture(name);
        }
        void WriteRenderTarget(RGTextureHandle h) { m_graph.RecordWrite(h, false); }
        void WriteDepthStencil(RGTextureHandle h) { m_graph.RecordWrite(h, true);  }
        // A render target with its load op (RG::Load). The graph transitions it to
        // RENDER_TARGET; the pass binds it in Execute. An invalid handle declares
        // nothing (HdrScene() under -rgimportoff).
        void RenderTarget(RGTextureHandle h, Load load) { m_graph.RecordRenderTarget(h, load); }
        // The pass reads @p h inside its own list and leaves it in @p state
        // (CopyHdrSceneToScoped: RENDERTARGET). See RecordScopedRead.
        void ReadScoped(RGTextureHandle h, RHI::ResourceState state) { m_graph.RecordScopedRead(h, state); }
        // This graph's HDR scene colour import (RG::kHdrSceneTexture), invalid when
        // the graph imports none (-rgimportoff).
        RGTextureHandle HdrScene() const { return m_graph.LookupTexture(kHdrSceneTexture); }
        void ReadSRV(RGTextureHandle h)            { m_graph.RecordRead(h);         }
        // A read of what @p h held at the END OF THE PREVIOUS FRAME: the pass is
        // declared before this frame's writer on purpose. Transitioned like ReadSRV;
        // the DAG gives it no RAW edge and never calls it a StaleRead, and still
        // orders it before the writer (WAR). RTReflectionPass reading last frame's
        // ReSTIR GI is one.
        void ReadHistory(RGTextureHandle h)        { m_graph.RecordRead(h, true);   }
        // ReadSRV / ReadHistory of pass textures by name
        // (RenderGraph/PassTextureNames.h). A name this graph has no producer of (a
        // preview graph, -rgimportoff) declares nothing.
        void ReadPassTextures(std::initializer_list<const char*> names)
        { for (const char* n : names) ReadSRV(GetTexture(n)); }
        void ReadPassTextureHistory(std::initializer_list<const char*> names)
        { for (const char* n : names) ReadHistory(GetTexture(n)); }
        // The pass binds @p h's DSV: what it does to each plane (RG::Depth,
        // RG::Stencil; see there), and @p stencilMask the stencil bits it tests or
        // writes - a field of RenderGraph/StencilBits.h that names the pass (the DAG,
        // StencilOverlap and StencilOwner go by it, and the device's tripwire holds
        // the pipelines to it). Bind the DSV with cl.ctx->DepthView(h). An invalid
        // handle declares nothing.
        void DepthStencil(RGTextureHandle h, Depth depth, Stencil stencil = Stencil::None,
                          uint8_t stencilMask = 0xFF)
        { m_graph.RecordDepthStencil(h, depth, stencil, stencilMask); }
        // A plane of a depth-stencil texture as an SRV; Plane::Depth is ReadSRV.
        void ReadSRV(RGTextureHandle h, Plane plane)
        { if (plane == Plane::Stencil) m_graph.RecordStencilRead(h); else m_graph.RecordRead(h); }
        // A copy's source / destination: @p plane of a depth-stencil texture,
        // the whole of any other. The pass records the copy.
        void CopySrc(RGTextureHandle h, Plane plane = Plane::Depth) { m_graph.RecordCopy(h, plane, false); }
        void CopyDst(RGTextureHandle h, Plane plane = Plane::Depth) { m_graph.RecordCopy(h, plane, true); }
        // Declares that the current pass binds @p h as a UAV: the graph creates the
        // physical texture with UNORDERED_ACCESS and transitions it to
        // UNORDERED_ACCESS before the pass runs.
        void WriteUAV(RGTextureHandle h)           { m_graph.RecordUAV(h);          }
        // See RenderGraph::RecordManualBarrier - an explicit opt-out, not a
        // shortcut. Pass @p entryState when the pass's own barriers assume a starting
        // state (OutlinePass / TracerRenderPass: DEPTHSTENCIL).
        void ManualBarrier(RGTextureHandle h,
                           RHI::ResourceState entryState = RHI::ResourceState::UNDEFINED)
        { m_graph.RecordManualBarrier(h, entryState); }
        // A shim: HdrSceneColor declares RenderTarget(HdrScene(), Clear) and
        // HdrSceneColorPreserve RenderTarget(HdrScene(), Load) when the graph imports
        // the HDR scene colour; Phase A still binds it.
        void SetColorTarget(BuiltinTexture t)      { m_graph.SetBuiltinTarget(t);   }

    private:
        RenderGraph& m_graph;
    };

    // ==== ImportDeclarer: passed to RenderPass::DeclareImports ====
    class ImportDeclarer
    {
    public:
        explicit ImportDeclarer(RenderGraph& graph) : m_graph(graph) {}
        // @p name: a constant of RenderGraph/PassTextureNames.h - the name its
        // readers look up. @p desc: what the snapshot reports (0 = the render
        // size); the graph never creates it. @p spec: the source, the entry and
        // the tracking (ImportSpec).
        RGTextureHandle Import(const char* name, const RGTextureDesc& desc, const ImportSpec& spec)
        {
            return m_graph.DeclarePassImport(name, desc, spec);
        }
        // Plane @p plane of @p history as two Graph-tracked imports: @p name, Cur
        // (Stale: its pass writes it this frame), and @p name + ".Prev", Prev
        // (History: last frame's content; its readers declare ReadHistory, or read
        // it scoped). Their sources are the history's own copies
        // (HistoryTexture::CurSource / PrevSource), which its BeginFrame keeps
        // current.
        HistoryHandles ImportHistory(const char* name, const RGTextureDesc& desc,
                                     const HistoryTexture& history, uint32_t plane = 0);

    private:
        RenderGraph& m_graph;
    };

} // namespace RG
