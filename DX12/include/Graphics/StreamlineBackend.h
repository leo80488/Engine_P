#pragma once

// StreamlineBackend - process-wide owner of the NVIDIA Streamline runtime.
//
// One backend serves every Streamline feature the engine uses: sl.dlss (DLSS SR / DLAA,
// DLSSPass), sl.dlss_d (Ray Reconstruction), sl.dlss_g (Frame Generation, SurfaceDX12's present
// path), sl.reflex and sl.pcl (SurfaceDX12 sets the Reflex mode; the frame loop sleeps and emits the
// latency markers). They share one set of per-frame constants and one resource-tag table, so they
// have one owner (FidelityFXBackend is per pass because FFX contexts are per effect).
//
// The SDK half is the Streamline module's (src/Streamline/StreamlineBackendSL.cpp), installed at
// process start. Without it - an exe that does not link the module, or no SDK
// (ENGINE_WITH_STREAMLINE=0) - every declaration still exists and the core's dormant backend
// reports NoSDK, so call sites never #if: they ask IsFeatureSupported() and take the dormant path, as
// on a non-NVIDIA GPU. To enable: external/Streamline/PLACE_SDK_HERE.md.
//
// Init uses eUseManualHooking: Streamline does not interpose device and factory creation; the host
// routes the interfaces it must see through UpgradeInterface itself. FidelityFX, NRD and Tracy then
// get the device proxy (SL proxies forward QueryInterface); GetNativeInterface returns the raw one.
//
// Call order:
//   1. Init()                     before CreateDXGIFactory2
//   2. UpgradeInterface(&factory) right after CreateDXGIFactory2 (a swap chain the upgraded
//                                 factory creates is already the SL proxy)
//   3. UpgradeInterface(&device), then SetD3DDevice(device): right after D3D12CreateDevice,
//                                 before any command queue exists
//   per frame, in this order:
//   4. NewFrameToken(frameIndex)  once, at the top of the frame loop (App::Run), before input is
//                                 sampled: Reflex needs the token before its sleep. Only once:
//                                 the CPU runs up to kFrameCount-1 frames ahead, and a second
//                                 token hands mismatched frame indices to constants, tags,
//                                 markers and evaluate - the most common cause of DLSS-G
//                                 rejecting a frame.
//   4b. PublishRenderToken(t)     at the game/render seam. After it use RenderFrameToken(),
//                                 before it GameFrameToken(): the same token without the
//                                 render-owner lane, one frame apart with it.
//   5. SetConstants(...)          game half, game token
//   6. TagResourcesForFrame(...)  on the thread recording the consuming CL, render token
//   7. EvaluateFeature(...)       likewise. slEvaluateFeature is the one entry point the SDK
//                                 documents as not thread safe, so DLSSPass takes its token as
//                                 a parameter instead of reading a latch.
//   8. Shutdown()                 after a full GPU drain, before device teardown

#include <atomic>
#include <mutex>
#include <cstdint>
#include <DirectXMath.h>

#ifndef ENGINE_WITH_STREAMLINE
#define ENGINE_WITH_STREAMLINE 0
#endif

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGISwapChain;

namespace SL
{
    // Mirrors sl::Feature for the subset the engine loads. Values are engine-local;
    // StreamlineBackendSL.cpp maps them to the SDK's.
    enum class Feature : uint32_t
    {
        DLSS = 0,   // Super Resolution / DLAA
        DLSS_G,     // Frame Generation
        DLSS_RR,    // Ray Reconstruction (sl.dlss_d): SR + guide-informed denoise
        Reflex,
        PCL,
        Count
    };

    const char* FeatureName(Feature f);

    // Why a feature is unavailable, shown verbatim in the editor tooltip so a greyed-out DLSS
    // explains itself.
    enum class FeatureStatus : uint32_t
    {
        Unknown = 0,
        Supported,
        NoSDK,           // ENGINE_WITH_STREAMLINE == 0
        InitFailed,      // slInit failed (missing/blocked DLLs)
        NoHardware,      // adapter lacks the required silicon
        DriverTooOld,
        OSUnsupported,
        NotLoaded,       // plugin DLL absent
        Denied,          // blocked by driver/OS policy (e.g. HW scheduling off)
        NotRequested,    // deliberately not in Init's load list - never probed
    };

    const char* FeatureStatusName(FeatureStatus s);

    // ==== Per-frame common constants (mirrors sl::Constants) ====
    // All matrices are row-major and unjittered. The engine's row-vector DirectXMath matrices pass
    // through unchanged, but without jitter: TAAJitterState jitters the raster projection, and SL
    // needs the clean one plus the jitter reported separately in jitterOffset.
    struct FrameConstants
    {
        DirectX::XMFLOAT4X4 cameraViewToClip{};   // projection (no jitter)
        DirectX::XMFLOAT4X4 clipToCameraView{};   // inverse projection
        DirectX::XMFLOAT4X4 clipToPrevClip{};     // reprojection cur -> prev
        DirectX::XMFLOAT4X4 prevClipToClip{};     // reprojection prev -> cur

        // Subpixel jitter in pixels at render resolution (the value fed to the projection matrix, so
        // projection and DLSS agree by construction).
        DirectX::XMFLOAT2 jitterOffset{ 0.0f, 0.0f };

        // Multiplier taking the engine's motion vectors into SL's convention: normalised so a
        // full-screen traversal is 1.0, pointing current -> previous. For the engine's raw NDC delta
        // (curNDC - prevNDC) that is (-0.5, +0.5); the derivation is at the assignment in
        // Renderer::UploadFrameData. A wrong value shows only as smearing during motion, easily misread
        // as "DLSS is blurry".
        DirectX::XMFLOAT2 mvecScale{ -0.5f, 0.5f };

        DirectX::XMFLOAT3 cameraPos{};
        DirectX::XMFLOAT3 cameraUp{};
        DirectX::XMFLOAT3 cameraRight{};
        DirectX::XMFLOAT3 cameraFwd{};

        float cameraNear  = 0.1f;
        float cameraFar   = 1000.0f;
        float cameraFOV   = 1.0f;     // vertical, radians
        float cameraAspectRatio = 1.777f;

        // Reversed-Z throughout this engine (near=1, far=0).
        bool depthInverted        = true;
        // GBuffer velocity includes camera motion (a full cur-prev clip delta), so SL must not add its
        // own camera term.
        bool cameraMotionIncluded = true;
        bool motionVectors3D      = false;
        bool orthographicProjection = false;
        // Set for one frame on a camera cut or an Edit<->Play transition: the signal that also resets
        // TAAPass and FSR2Pass history.
        bool reset                = false;
    };

    // ==== Resource tagging ====
    enum class BufferType : uint32_t
    {
        Depth = 0,
        MotionVectors,
        ScalingInputColor,    // render-res HDR scene  (DLSS SR input)
        ScalingOutputColor,   // display-res           (DLSS SR output)
        HUDLessColor,         // FG: scene without UI, MUST match backbuffer size
        UIColorAndAlpha,      // FG: UI only,          MUST match backbuffer size
        Backbuffer,
        Albedo,
        SpecularAlbedo,
        NormalRoughness,
        Roughness,
        SpecularHitDistance,
        DiffuseHitDistance,
        Exposure,
        Normals,              // RR: the packed normal + roughness guide, tagged as plain normals (sl 5, not 14)
        ColorBeforeTransparency,   // RR guide (sl 40)
        BiasCurrentColor,     // DLSS SR's lerp(history, current, bias) hint (sl 29)
    };

    // How long SL may rely on the tagged memory.
    //   ValidUntilEvaluate - consumed inside slEvaluateFeature (DLSS SR inputs)
    //   ValidUntilPresent  - consumed at Present (all Frame Generation inputs)
    //   OnlyValidNow       - SL copies it immediately (costs bandwidth; avoid)
    enum class ResourceLife : uint32_t
    {
        OnlyValidNow = 0,
        ValidUntilPresent,
        ValidUntilEvaluate,
    };

    struct ResourceTag
    {
        BufferType      type     = BufferType::Depth;
        ID3D12Resource* resource = nullptr;
        // Native D3D12_RESOURCE_STATES the resource is in when tagged; a plain uint32_t so this header
        // does not pull in d3d12.h.
        uint32_t        state    = 0;
        // Sub-rect; all zero means the whole resource, which every current call site passes.
        uint32_t        left = 0, top = 0, width = 0, height = 0;
        ResourceLife    life     = ResourceLife::ValidUntilEvaluate;
    };

    // ==== DLSS Super Resolution ====
    enum class DLSSMode : uint32_t
    {
        Off = 0,
        MaxQuality,        // ~1.50x
        Balanced,          // ~1.72x
        MaxPerformance,    // ~2.00x
        UltraPerformance,  // ~3.00x
        DLAA,              // 1:1, anti-aliasing only
    };

    const char* DLSSModeName(DLSSMode m);

    // Which DL model DLSS runs. Mirrors sl::DLSSPreset, restricted to the values the SDK still
    // honours (A-D are removed; E-I and N-O are deprecated or revert to the default).
    // Not cosmetic: the SDK's per-mode defaults are K for DLAA/Quality/Balanced, M for Performance
    // and L for UltraPerformance, and K ghosts more and is less sharp than L/M, so a thin dark feature
    // such as the character outline reads softer exactly in the K modes. Default is also an OTA
    // hazard: sl_dlss.h says eDefault may change after an OTA, and Game.exe enables OTA.
    enum class DLSSPreset : uint32_t
    {
        Default = 0,   // whatever the driver picks for the mode; can change via OTA
        J,             // transformer, slightly less ghosting than K but more flicker
        K,             // transformer, SDK default for DLAA/Quality/Balanced
        L,             // sharper + more stable than J/K, costs more
        M,             // ~L's quality, closer to J/K in speed
    };

    const char* DLSSPresetName(DLSSPreset p);

    struct DLSSOptimalSettings
    {
        uint32_t renderW = 0, renderH = 0;   // authoritative render resolution
        uint32_t minW = 0, minH = 0;         // dynamic-resolution bounds
        uint32_t maxW = 0, maxH = 0;
        float    sharpness = 0.0f;
    };

    // ==== DLSS Frame Generation ====
    enum class FrameGenMode : uint32_t
    {
        Off = 0,
        On,        // fixed multiplier, see numFramesToGenerate
        Auto,      // sl::DLSSGMode::eAuto
        Dynamic,   // multiplier tracks display refresh; needs a recent driver
    };

    struct FrameGenState
    {
        bool     enabled              = false;
        // Frames presented since the previous GetFrameGenState call (the SDK resets the counter on
        // every query). It is not "frames being generated": divide it by the frames rendered in the
        // same window.
        uint32_t framesPresentedSincePoll = 0;
        uint32_t numFramesToGenerateMax = 0;   // 1 on Ada, higher on Blackwell
        // Non-zero only with the eRequestVRAMEstimate flag; without it DLSS-G does not compute the
        // figure and this reads 0, as if Frame Generation cost no memory.
        uint64_t estimatedVRAMBytes   = 0;
        // Dynamic Multi-Frame Generation: the multiplier tracks the display refresh. Needs hardware
        // capable of >2x and a recent driver, so it is reported, not assumed.
        bool     dynamicMFGSupported  = false;
        // Whether DLSS-G can honour VSync in the current configuration; the UI must not offer the
        // combination when it cannot.
        bool     vsyncSupported       = false;
        bool     statusOk             = false;
        // Set when statusOk is false and shown in the editor, not only logged: Frame Generation failing
        // silently while the user believes it is on is worse than never turning on.
        const char* statusText        = "";
    };

    // ==== Reflex ====
    enum class ReflexMode : uint32_t
    {
        Off = 0,
        LowLatency,
        LowLatencyWithBoost,
    };

    // Order matters: SL validates that markers arrive in this sequence with frame indices matching
    // the constants for the same token.
    enum class LatencyMarker : uint32_t
    {
        SimulationStart = 0,
        SimulationEnd,
        RenderSubmitStart,
        RenderSubmitEnd,
        PresentStart,
        PresentEnd,
        // No input-sample marker: eInputSample (6) is deprecated, and ePCLatencyPing (8) is the
        // driver's latency ping, not input sampling. To mark input, add eControllerInputSample (13)
        // and emit it at the input site.
    };

    // The DLSS-RR transformer models of Streamline 2.14.1 / DLSS 310.9.1: D, E and F. F is the latest
    // and default (NVIDIA's "RR2"): it adds the Responsivity Mask and the DoF guide and costs more.
    // The numbers are sl::DLSSDPreset's.
    enum class DLSSRRModel : uint32_t { D=4, E=5, F=6 };
    inline const char* DLSSRRModelName(DLSSRRModel m) { return m==DLSSRRModel::F?"F":(m==DLSSRRModel::E?"E":"D"); }

    struct InitDesc
    {
        // Directory to load sl.*.dll from. Empty = next to the executable.
        const wchar_t* pluginPath = nullptr;
        // Verify the Authenticode signature of sl.interposer.dll before loading it. Keep it on: that DLL
        // gets control of Present, so a swapped copy owns the frame, and the check costs ~35 ms warm
        // (WinVerifyTrust). Set false only for an unsigned development SDK build; nothing turns it off
        // automatically.
        bool  verifySignature = true;

        // Let Streamline fetch and prefer over-the-air plugin updates (PreferenceFlags::eAllowOTA |
        // eLoadDownloadedPlugins). Off by default: for a build that ships its own sl.* / nvngx_* DLLs it
        // is pure boot cost, ~1.2 s (checkForOTA ~200 ms, then ~1.0 s reconciling the
        // %PROGRAMDATA%/NVIDIA/NGX copies with ours; SL_LOG=1 shows it). GraphicsDX12 sets it to
        // !Sys::IsEditorHost(): the shipped Game.exe gets a newer DLSS over the air, the editor
        // (relaunched many times a day) does not.
        bool  allowOTA        = false;
        // Emit SL's own diagnostics through the engine logger.
        bool  enableLogging   = false;
        // Load the DLSS-RR plugin (sl.dlss_d). Off by default, so a caller that says nothing pays no RR
        // DLL-load cost; GraphicsDX12 decides from -dlss_rr / -dlss_rr_probe and the
        // rayReconstructionLoad setting.
        bool requestRayReconstruction = false;

        // ---- Application identity ----
        // Streamline needs an NVIDIA-issued applicationId or the engine triple (type + engineVersion +
        // projectId). With neither, the production DLLs refuse the NGX features (DLSS, DLSS-G report
        // eErrorFeatureNotSupported) while Reflex and PCL still come up: only the NGX features missing
        // means an identity problem, not a hardware one. The development DLLs skip the check and burn a
        // "DO NOT DISTRIBUTE" watermark into the image.
        uint32_t    applicationId = 0;      // 0 = fall back to the triple below
        const char* projectId     = nullptr;  // GUID string
        const char* engineVersion = nullptr;
    };

    // ==== Backend ====
    class StreamlineBackend
    {
    public:
        // Process-wide: Streamline's own state is process-wide (slInit / slShutdown are global). The
        // installed backend - the Streamline module's, which holds the SDK - else the core's dormant one,
        // whose features all report NoSDK.
        static StreamlineBackend& Get();
        // The Streamline module's backend, from its OnProcessStart: before GraphicsDX12 exists, since Init
        // must run before the DXGI factory is created. Null puts the dormant one back.
        static void Install(StreamlineBackend* backend);

        virtual ~StreamlineBackend() = default;
        StreamlineBackend(const StreamlineBackend&)            = delete;
        StreamlineBackend& operator=(const StreamlineBackend&) = delete;

        // Call before CreateDXGIFactory2. Returns false if the SDK is absent or slInit failed; that is a
        // normal outcome and the caller carries on without DLSS.
        virtual bool Init(const InitDesc& desc);
        virtual void Shutdown();
        bool IsInitialized() const { return m_initialized; }

        // Right after D3D12CreateDevice. Also runs the per-adapter feature support queries, so
        // IsFeatureSupported() is meaningful only after it.
        virtual void SetD3DDevice(ID3D12Device* device);

        // Replaces *inOutSwapChain by its SL proxy and releases the original; downstream Present,
        // ResizeBuffers and GetBuffer then flow through SL unchanged. No caller: GraphicsDX12 upgrades the
        // factory, whose swap chains are already proxies, and upgrading only the swap chain is not enough
        // (see UpgradeInterface).
        virtual bool UpgradeSwapChain(IDXGISwapChain** inOutSwapChain);

        // Replaces *inOutIface by its SL proxy. Manual hooking makes the host route Streamline's
        // mandatory hook set (sl_hooks.h FunctionHookID): IDXGIFactory::CreateSwapChain*,
        // ID3D12Device::CreateCommandQueue and the IDXGISwapChain calls (Present, GetBuffer,
        // ResizeBuffers, GetCurrentBackBufferIndex, SetFullscreenState). Upgrading only the swap chain
        // leaves DLSS-G loaded, configured and reporting eDLSSGStatusOk while it never installs its
        // Present path and generates nothing.
        virtual bool UpgradeInterface(void** inOutIface);

        bool          IsFeatureSupported(Feature f) const;
        FeatureStatus GetFeatureStatus(Feature f) const;
        // Human-readable reason, for the editor tooltip.
        const char*   GetFeatureStatusText(Feature f) const;

        // Frame tokens are per half; there is no global "current" token. With the render-owner lane the
        // halves are on different frames at once, and a tag or marker filed under the wrong frame does
        // not fail: SL accepts it, and the cost shows later as Reflex holding too long or Frame
        // Generation pacing badly. Two tokens in flight is supported: Init sets
        // eUseFrameBasedResourceTagging, which sl_core_api.h allows for up to 3 frames in flight.
        // slSetTagForFrame, slSetConstants, slGetNewFrameToken, slPCLSetMarker and slReflexSleep are
        // documented thread safe; slEvaluateFeature is not, so DLSSPass takes its token as a parameter
        // on the one thread that evaluates.
        virtual void* NewFrameToken(uint32_t frameIndex);   // game half: mints frame F's token
        virtual void* GameFrameToken() const;               // game half reads it back
        virtual void  PublishRenderToken(void* token);      // the seam: hands F to the render half
        virtual void* RenderFrameToken() const;             // render half reads what the seam published

        // The frame index a token was minted for: sl::FrameToken's only public member is
        // operator uint32_t() (sl_core_types.h), so a site using the wrong frame's token can be asserted
        // against. Returns UINT32_MAX for a null token or a dormant build, so callers compare without
        // special-casing the disabled path.
        static uint32_t TokenFrameIndex(void* token);
        // The installed backend's answer for TokenFrameIndex.
        virtual uint32_t FrameIndexOf(void* token) const;

        virtual bool SetConstants(const FrameConstants& c, void* frameToken);

        // @p cl may be null for ValidUntilPresent tags that need no state transition.
        virtual bool TagResourcesForFrame(const ResourceTag* tags, uint32_t count,
                                  void* frameToken,
                                  ID3D12GraphicsCommandList* cl);

        // Records the feature's work into @p cl. The caller restores command-list state afterwards
        // (GraphicsDX12::RebindShaderVisibleHeaps, as on the FFX path).
        virtual bool EvaluateFeature(Feature f, void* frameToken,
                             ID3D12GraphicsCommandList* cl);
        // Caller must drain all GPU work using the viewport before releasing.
        virtual bool FreeFeatureResources(Feature f);

        // Native (non-proxy) interface behind an SL proxy, for code outside SL that needs the raw
        // pointer.
        virtual void* GetNativeInterface(void* proxy) const;

        // ---- DLSS Super Resolution ----
        // The render resolution DLSS wants for this output size and mode. It is the authority: do not
        // derive the render size from GetUpscaleRatio() while DLSS upscales, because DLSS checks the
        // render extent against its own table and rejects a mismatch. Safe before any DLSS feature
        // exists.
        virtual bool GetDLSSOptimalSettings(DLSSMode mode, uint32_t outW, uint32_t outH,
                                    DLSSOptimalSettings& out) const;
        // The same for DLSS Ray Reconstruction (slDLSSDGetOptimalSettings): RR upscales from its own
        // table. False without the RR plugin.
        virtual bool GetDLSSDOptimalSettings(DLSSMode mode, uint32_t outW, uint32_t outH,
                                     DLSSOptimalSettings& out) const;
        // RR's own estimate of its VRAM (slDLSSDGetState).
        virtual bool GetDLSSDVRAMBytes(uint64_t& out) const;

        // Creates the DLSS feature on first call and reconfigures it after.
        // Main thread only: it can allocate GPU resources.
        virtual bool SetDLSSOptions(DLSSMode mode, uint32_t outW, uint32_t outH,
                            float sharpness, bool useAutoExposure,
                            DLSSPreset preset = DLSSPreset::Default);
        // Main thread; viewport options are serialized. Requires matching SR options. Normals are
        // world-space with linear roughness in alpha; matrices are unjittered.
        virtual bool SetDLSSDOptions(DLSSMode mode,uint32_t outW,uint32_t outH,
            const DirectX::XMFLOAT4X4& worldToView,const DirectX::XMFLOAT4X4& viewToWorld,
            DLSSRRModel model=DLSSRRModel::D, float exposureScale=1.f, float preExposure=1.f);

        // ---- DLSS Frame Generation ----
        // Main thread, at a frame boundary: toggling rebuilds the swap chain internally, so never call it
        // mid-frame; route it through SurfaceDX12's pending-swap-chain-ops latch. @p hudLessFormat and
        // @p uiFormat are the native DXGI formats of the tagged HUD-less and UI buffers; declaring them
        // lets HDR10 work (FP16 composition, RGB10A2 back buffer). 0 leaves a format unspecified.
        virtual bool SetFrameGenOptions(FrameGenMode mode, uint32_t numFramesToGenerate,
                                uint32_t hudLessFormat = 0, uint32_t uiFormat = 0);
        virtual bool GetFrameGenState(FrameGenState& out) const;

        // ---- Reflex + PCL ----
        // @p frameLimitUs is Reflex's own frame limiter (0 = uncapped).
        virtual bool SetReflexOptions(ReflexMode mode, uint32_t frameLimitUs);

        // True while Reflex paces the frame. The engine's waitable-timer limiter must stand down then:
        // two limiters on one frame add the latency Reflex exists to remove.
        bool IsReflexPacing() const
        {
            return m_initialized && m_reflexMode != ReflexMode::Off &&
                   m_status[static_cast<uint32_t>(Feature::Reflex)] == FeatureStatus::Supported;
        }
        ReflexMode GetReflexMode() const { return m_reflexMode; }
        // Blocks until Reflex lets the frame start. At the very top of the frame loop, before input is
        // sampled.
        virtual bool ReflexSleep(void* frameToken);
        virtual bool SetLatencyMarker(LatencyMarker marker, void* frameToken);

    protected:
        StreamlineBackend() = default;

        bool          m_initialized  = false;
        bool          m_deviceSet    = false;
        // Last mode handed to SetFrameGenOptions. slDLSSGGetState's options parameter is an input ("the
        // state for these options"), so the mode cannot be read back from it.
        FrameGenMode  m_fgMode       = FrameGenMode::Off;
        ReflexMode    m_reflexMode   = ReflexMode::Off;
        // Atomic: SetLatencyMarker runs on both halves at once (SimulationStart/End on the game thread,
        // RenderSubmitEnd and the Present pair on the render thread), and they share this one
        // suppression flag.
        std::atomic<bool>  m_loggedMarkerFailure { false };
        ID3D12Device*      m_device       = nullptr;
        // One token per half; see NewFrameToken for why there is no single current token.
        std::atomic<void*> m_tokenGame    { nullptr };
        std::atomic<void*> m_tokenRender  { nullptr };
        FeatureStatus m_status[static_cast<uint32_t>(Feature::Count)] = {};
        // Serializes the sl*SetOptions entry points against each other and against GetFrameGenState,
        // GetDLSSDVRAMBytes and FreeFeatureResources. Streamline keeps one option set per viewport, so a
        // game-thread settings apply and a render-thread frame-gen suspend can interleave inside the SDK and
        // leave it configured for neither. Mutable: the const queries take it too.
        mutable std::mutex m_optionsMutex;

        // The features Init asked slInit to load. QueryFeatureSupport probes only these: an unrequested
        // probe returns eErrorFeatureMissing, which FromSLResult would misreport as NotLoaded ("plugin
        // DLL not found").
        bool m_requested[static_cast<uint32_t>(Feature::Count)] = {};
    };

} // namespace SL
