// StreamlineBackendSL - the Streamline SDK behind SL::StreamlineBackend (Graphics/StreamlineBackend.h),
// the Streamline module's. Installed at process start (StreamlineModule.cpp), before GraphicsDX12
// creates the DXGI factory: slInit must run first. Without the SDK (ENGINE_WITH_STREAMLINE=0) nothing
// here is compiled and the core's dormant backend stays.

#include "Graphics/StreamlineBackend.h"
#include "System/Log.h"
#include "System/Check.h"
#include "System/BootTimeline.h"

#include <cstring>
#include <cmath>

#if ENGINE_WITH_STREAMLINE
    #include <sl.h>
    #include <sl_consts.h>
    #include <sl_dlss.h>
    #include <sl_dlss_g.h>
    #include <sl_dlss_d.h>       // Ray Reconstruction (kFeatureDLSS_RR)
    #include <sl_reflex.h>
    #include <sl_pcl.h>
    #include <sl_security.h>

#pragma comment(lib, "sl.interposer.lib")

namespace SL
{

class StreamlineBackendSL final : public StreamlineBackend
{
public:
    bool Init(const InitDesc& desc) override;
    void Shutdown() override;
    void SetD3DDevice(ID3D12Device* device) override;
    bool UpgradeSwapChain(IDXGISwapChain** inOutSwapChain) override;
    bool UpgradeInterface(void** inOutIface) override;

    void* NewFrameToken(uint32_t frameIndex) override;
    void* GameFrameToken() const override;
    void  PublishRenderToken(void* token) override;
    void* RenderFrameToken() const override;
    uint32_t FrameIndexOf(void* token) const override;

    bool SetConstants(const FrameConstants& c, void* frameToken) override;
    bool TagResourcesForFrame(const ResourceTag* tags, uint32_t count, void* frameToken,
                              ID3D12GraphicsCommandList* cl) override;
    bool EvaluateFeature(Feature f, void* frameToken, ID3D12GraphicsCommandList* cl) override;
    bool FreeFeatureResources(Feature f) override;
    void* GetNativeInterface(void* proxy) const override;

    bool GetDLSSOptimalSettings(DLSSMode mode, uint32_t outW, uint32_t outH,
                                DLSSOptimalSettings& out) const override;
    bool GetDLSSDOptimalSettings(DLSSMode mode, uint32_t outW, uint32_t outH,
                                 DLSSOptimalSettings& out) const override;
    bool GetDLSSDVRAMBytes(uint64_t& out) const override;
    bool SetDLSSOptions(DLSSMode mode, uint32_t outW, uint32_t outH, float sharpness, bool useAutoExposure,
                        DLSSPreset preset) override;
    bool SetDLSSDOptions(DLSSMode mode, uint32_t outW, uint32_t outH,
                         const DirectX::XMFLOAT4X4& worldToView, const DirectX::XMFLOAT4X4& viewToWorld,
                         DLSSRRModel model, float exposureScale, float preExposure) override;
    bool SetFrameGenOptions(FrameGenMode mode, uint32_t numFramesToGenerate, uint32_t hudLessFormat,
                            uint32_t uiFormat) override;
    bool GetFrameGenState(FrameGenState& out) const override;
    bool SetReflexOptions(ReflexMode mode, uint32_t frameLimitUs) override;
    bool ReflexSleep(void* frameToken) override;
    bool SetLatencyMarker(LatencyMarker marker, void* frameToken) override;

private:
    void QueryFeatureSupport();
};


namespace
{
    // The engine tags whole resources; a single viewport is all we ever use.
    const sl::ViewportHandle kViewport{ 0u };

    sl::Feature ToSLFeature(Feature f)
    {
        switch (f)
        {
            case Feature::DLSS:   return sl::kFeatureDLSS;
            case Feature::DLSS_G: return sl::kFeatureDLSS_G;
            case Feature::DLSS_RR: return sl::kFeatureDLSS_RR;
            case Feature::Reflex: return sl::kFeatureReflex;
            case Feature::PCL:    return sl::kFeaturePCL;
            default:              return sl::kFeatureDLSS;
        }
    }

    sl::BufferType ToSLBufferType(BufferType t)
    {
        switch (t)
        {
            case BufferType::Depth:              return sl::kBufferTypeDepth;
            case BufferType::MotionVectors:      return sl::kBufferTypeMotionVectors;
            case BufferType::ScalingInputColor:  return sl::kBufferTypeScalingInputColor;
            case BufferType::ScalingOutputColor: return sl::kBufferTypeScalingOutputColor;
            case BufferType::HUDLessColor:       return sl::kBufferTypeHUDLessColor;
            case BufferType::UIColorAndAlpha:    return sl::kBufferTypeUIColorAndAlpha;
            case BufferType::Backbuffer:         return sl::kBufferTypeBackbuffer;
            case BufferType::Albedo:             return sl::kBufferTypeAlbedo;
            case BufferType::SpecularAlbedo:     return sl::kBufferTypeSpecularAlbedo;
            case BufferType::NormalRoughness:    return sl::kBufferTypeNormalRoughness;
            case BufferType::Roughness:          return sl::kBufferTypeRoughness;
            case BufferType::SpecularHitDistance:return sl::kBufferTypeSpecularHitDistance;
            case BufferType::Exposure: return sl::kBufferTypeExposure;
            case BufferType::DiffuseHitDistance: return sl::kBufferTypeDiffuseHitDistance;
            case BufferType::Normals:            return sl::kBufferTypeNormals;
            case BufferType::ColorBeforeTransparency: return sl::kBufferTypeColorBeforeTransparency;
            case BufferType::BiasCurrentColor:   return sl::kBufferTypeBiasCurrentColorHint;
            default:                             return sl::kBufferTypeDepth;
        }
    }

    sl::ResourceLifecycle ToSLLifecycle(ResourceLife l)
    {
        switch (l)
        {
            case ResourceLife::ValidUntilPresent:  return sl::ResourceLifecycle::eValidUntilPresent;
            case ResourceLife::ValidUntilEvaluate: return sl::ResourceLifecycle::eValidUntilEvaluate;
            case ResourceLife::OnlyValidNow:
            default:                               return sl::ResourceLifecycle::eOnlyValidNow;
        }
    }

    // Maps an sl::Result onto the engine's "why is this greyed out" vocabulary. Anything
    // unrecognised becomes NoHardware, never Supported: an unknown failure must not present as a
    // working feature.
    FeatureStatus FromSLResult(sl::Result r)
    {
        switch (r)
        {
            case sl::Result::eOk:                            return FeatureStatus::Supported;
            case sl::Result::eErrorNoSupportedAdapterFound:  return FeatureStatus::NoHardware;
            case sl::Result::eErrorAdapterNotSupported:      return FeatureStatus::NoHardware;
            case sl::Result::eErrorDriverOutOfDate:          return FeatureStatus::DriverTooOld;
            case sl::Result::eErrorOSOutOfDate:              return FeatureStatus::OSUnsupported;
            case sl::Result::eErrorOSDisabledHWS:            return FeatureStatus::OSUnsupported;
            case sl::Result::eErrorFeatureMissing:           return FeatureStatus::NotLoaded;
            case sl::Result::eErrorFeatureNotSupported:      return FeatureStatus::NoHardware;
            default:                                         return FeatureStatus::NoHardware;
        }
    }

    void SLLogCallback(sl::LogType type, const char* msg)
    {
        if (!msg) return;
        switch (type)
        {
            case sl::LogType::eError: LOG_ERROR("Streamline: %s", msg);   break;
            case sl::LogType::eWarn:  LOG_WARNING("Streamline: %s", msg); break;
            default:                  LOG_INFO("Streamline: %s", msg);    break;
        }
    }

    // Both are row-major 4x4 float; copy rather than field-by-field so a layout
    // change in either fails loudly at the static_assert instead of silently
    // transposing the reprojection matrix.
    void CopyMatrix(sl::float4x4& dst, const DirectX::XMFLOAT4X4& src)
    {
        static_assert(sizeof(sl::float4x4) == sizeof(DirectX::XMFLOAT4X4),
                      "sl::float4x4 / XMFLOAT4X4 size mismatch");
        std::memcpy(&dst, &src, sizeof(dst));
    }

    constexpr sl::Boolean SLBool(bool b)
    {
        return b ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    }

    // sl::Result spelled out: the codes otherwise surface as bare integers (result=20 is
    // eErrorMissingInputParameter).
    const char* ResultName(sl::Result r)
    {
        switch (r)
        {
            case sl::Result::eOk:                          return "eOk";
            case sl::Result::eErrorIO:                     return "eErrorIO";
            case sl::Result::eErrorDriverOutOfDate:        return "eErrorDriverOutOfDate";
            case sl::Result::eErrorOSOutOfDate:            return "eErrorOSOutOfDate";
            case sl::Result::eErrorOSDisabledHWS:          return "eErrorOSDisabledHWS (enable hardware GPU scheduling)";
            case sl::Result::eErrorDeviceNotCreated:       return "eErrorDeviceNotCreated";
            case sl::Result::eErrorNoSupportedAdapterFound:return "eErrorNoSupportedAdapterFound";
            case sl::Result::eErrorAdapterNotSupported:    return "eErrorAdapterNotSupported";
            case sl::Result::eErrorNoPlugins:              return "eErrorNoPlugins";
            case sl::Result::eErrorDXGIAPI:                return "eErrorDXGIAPI";
            case sl::Result::eErrorD3DAPI:                 return "eErrorD3DAPI";
            case sl::Result::eErrorNVAPI:                  return "eErrorNVAPI";
            case sl::Result::eErrorReflexAPI:              return "eErrorReflexAPI";
            case sl::Result::eErrorNGXFailed:              return "eErrorNGXFailed";
            case sl::Result::eErrorJSONParsing:            return "eErrorJSONParsing";
            case sl::Result::eErrorMissingProxy:           return "eErrorMissingProxy (swap chain not upgraded)";
            case sl::Result::eErrorMissingResourceState:   return "eErrorMissingResourceState (a tag omitted its D3D12 state)";
            case sl::Result::eErrorInvalidIntegration:     return "eErrorInvalidIntegration";
            case sl::Result::eErrorMissingInputParameter:  return "eErrorMissingInputParameter (viewport/tags/constants not supplied)";
            case sl::Result::eErrorNotInitialized:         return "eErrorNotInitialized";
            case sl::Result::eErrorComputeFailed:          return "eErrorComputeFailed";
            case sl::Result::eErrorInitNotCalled:          return "eErrorInitNotCalled";
            case sl::Result::eErrorInvalidParameter:       return "eErrorInvalidParameter";
            case sl::Result::eErrorMissingConstants:       return "eErrorMissingConstants";
            case sl::Result::eErrorDuplicatedConstants:    return "eErrorDuplicatedConstants";
            case sl::Result::eErrorMissingOrInvalidAPI:    return "eErrorMissingOrInvalidAPI";
            case sl::Result::eErrorCommonConstantsMissing: return "eErrorCommonConstantsMissing (slSetConstants not called for this frame/viewport)";
            case sl::Result::eErrorUnsupportedInterface:   return "eErrorUnsupportedInterface";
            case sl::Result::eErrorFeatureMissing:         return "eErrorFeatureMissing";
            case sl::Result::eErrorFeatureNotSupported:    return "eErrorFeatureNotSupported";
            case sl::Result::eErrorFeatureMissingHooks:    return "eErrorFeatureMissingHooks";
            case sl::Result::eErrorFeatureFailedToLoad:    return "eErrorFeatureFailedToLoad";
            case sl::Result::eErrorFeatureMissingDependency: return "eErrorFeatureMissingDependency";
            case sl::Result::eErrorInvalidState:           return "eErrorInvalidState";
            case sl::Result::eWarnOutOfVRAM:               return "eWarnOutOfVRAM";
            default:                                       return "<unrecognised sl::Result>";
        }
    }
}

bool StreamlineBackendSL::Init(const InitDesc& desc)
{
    if (m_initialized) return true;
    // Boot timeline: streamlineBegin -> streamlineInit spans slInit alone (signature check + plugin
    // load), apart from the window creation before it.
    Sys::Boot::Mark("streamlineBegin");

    for (auto& s : m_status) s = FeatureStatus::Unknown;

    // Signature check first: sl.interposer.dll gets control of Present (see InitDesc::verifySignature).
    if (desc.verifySignature)
    {
        // verifyEmbeddedSignature requires an absolute path: a relative one is rejected and looks exactly
        // like a failed signature, disabling DLSS everywhere. The DLL is copied next to the executable
        // at post-build, so the path is derived from the module.
        wchar_t path[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        bool haveDir = (n > 0 && n < MAX_PATH);
        if (haveDir)
        {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) *(slash + 1) = L'\0';
            else haveDir = false;
        }
        if (!haveDir || wcslen(path) + wcslen(L"sl.interposer.dll") >= MAX_PATH)
        {
            LOG_ERROR("Streamline: could not resolve the executable directory — refusing to load");
            for (auto& s : m_status) s = FeatureStatus::InitFailed;
            return false;
        }
        wcscat_s(path, MAX_PATH, L"sl.interposer.dll");

        if (!sl::security::verifyEmbeddedSignature(path))
        {
            LOG_ERROR("Streamline: signature verification failed for %ls — refusing to load. "
                      "(Development SDK builds are unsigned; set InitDesc::verifySignature=false "
                      "when using them.)", path);
            for (auto& s : m_status) s = FeatureStatus::InitFailed;
            return false;
        }
    }

    // Features this build asks slInit to load. This engine-level list is the single source: the sl::
    // array below derives from it, and QueryFeatureSupport reads m_requested to tell "never asked"
    // from "the driver said no". RR loads only when InitDesc::requestRayReconstruction asks, which
    // keeps its DLL-load cost off boots that do not use it.
    static constexpr Feature kRequested[] = {
        Feature::DLSS,
        Feature::DLSS_G,
        Feature::Reflex,
        Feature::PCL,
        Feature::DLSS_RR,
    };
    constexpr uint32_t kNumRequested =
        static_cast<uint32_t>(sizeof(kRequested) / sizeof(kRequested[0]));

    // static: slInit is handed the pointer, so the storage outlives this function.
    static sl::Feature s_featuresToLoad[kNumRequested]{};
    uint32_t requestedCount=0;
    for (auto& q : m_requested) q = false;
    for (uint32_t i = 0; i < kNumRequested; ++i)
    {
        if(kRequested[i]==Feature::DLSS_RR && !desc.requestRayReconstruction) continue;
        s_featuresToLoad[requestedCount++] = ToSLFeature(kRequested[i]);
        m_requested[static_cast<uint32_t>(kRequested[i])] = true;
    }

    sl::Preferences prefs{};
    prefs.showConsole      = false;
    prefs.logLevel         = desc.enableLogging ? sl::LogLevel::eDefault : sl::LogLevel::eOff;
    prefs.logMessageCallback = desc.enableLogging ? &SLLogCallback : nullptr;
    prefs.featuresToLoad   = s_featuresToLoad;
    prefs.numFeaturesToLoad = requestedCount;
    prefs.renderAPI        = sl::RenderAPI::eD3D12;
    prefs.engine           = sl::EngineType::eCustom;
    prefs.applicationId    = desc.applicationId;
    prefs.projectId        = desc.projectId;
    prefs.engineVersion    = desc.engineVersion;

    // Preferences::flags defaults to eDisableCLStateTracking | eAllowOTA | eLoadDownloadedPlugins, and
    // assigning it replaces that default, so what is still wanted is restated here; the OTA pair
    // deliberately is not (see below).
    // eDisableCLStateTracking: do not drop it. Without it Streamline restores command-list state
    // itself, with a barrier to PIXEL_SHADER_RESOURCE (0x80) - illegal on the compute command list the
    // upscaler runs on: Close() returns E_INVALIDARG and the frame is dropped (a black screen). With
    // it the host restores state (RebindShaderVisibleHeaps; every pass rebinds its PSO and root
    // signature).
    // eUseManualHooking: the host upgrades the interfaces Streamline must see (UpgradeInterface)
    // instead of SL interposing device and factory creation.
    // eUseFrameBasedResourceTagging: tags are scoped to a frame token, which keeps them correct while
    // the CPU runs frames ahead.
    prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking
                | sl::PreferenceFlags::eUseManualHooking
                | sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    // eAllowOTA / eLoadDownloadedPlugins are dropped by choice, not necessity: opt-in through
    // InitDesc::allowOTA (off by default, ~1.2 s per boot when on; see InitDesc::allowOTA).
    if (desc.allowOTA)
        prefs.flags = prefs.flags | sl::PreferenceFlags::eAllowOTA
                                  | sl::PreferenceFlags::eLoadDownloadedPlugins;

    if (desc.pluginPath)
    {
        static const wchar_t* s_paths[1];
        s_paths[0] = desc.pluginPath;
        prefs.pathsToPlugins    = s_paths;
        prefs.numPathsToPlugins = 1;
    }

    const sl::Result r = slInit(prefs, sl::kSDKVersion);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slInit failed (result=%d) — DLSS and Frame Generation unavailable",
                    static_cast<int>(r));
        const FeatureStatus s = FromSLResult(r);
        for (auto& st : m_status) st = (s == FeatureStatus::Supported) ? FeatureStatus::InitFailed : s;
        return false;
    }

    m_initialized = true;
    LOG_INFO("Streamline: initialised (manual hooking, frame-based tagging)");
    Sys::Boot::Mark("streamlineInit");   // end of the slInit span (bootPhasesMs)
    return true;
}

void StreamlineBackendSL::Shutdown()
{
    if (!m_initialized) return;
    const sl::Result r = slShutdown();
    if (r != sl::Result::eOk)
        LOG_WARNING("Streamline: slShutdown returned %d", static_cast<int>(r));
    m_initialized  = false;
    m_deviceSet    = false;
    m_device       = nullptr;
    m_tokenGame.store(nullptr, std::memory_order_release);
    // Both tokens, not just the game one: a render half that outlived Shutdown would otherwise
    // dereference a freed SDK token.
    m_tokenRender.store(nullptr, std::memory_order_release);
    for (auto& s : m_status) s = FeatureStatus::Unknown;
    LOG_INFO("Streamline: shut down");
}

void StreamlineBackendSL::SetD3DDevice(ID3D12Device* device)
{
    if (!m_initialized || !device) return;
    const sl::Result r = slSetD3DDevice(device);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slSetD3DDevice failed: %s", ResultName(r));
        for (auto& s : m_status) s = FeatureStatus::InitFailed;
        return;
    }
    m_device    = device;
    m_deviceSet = true;
    QueryFeatureSupport();
}

void StreamlineBackendSL::QueryFeatureSupport()
{
    // A default-constructed AdapterInfo asks the generic "is this possible on
    // this OS/driver at all" question. That is the right granularity here: the
    // engine has already committed to an adapter by this point, and SL resolves
    // the adapter from the device we just handed it.
    sl::AdapterInfo adapter{};

    for (uint32_t i = 0; i < static_cast<uint32_t>(Feature::Count); ++i)
    {
        const Feature f = static_cast<Feature>(i);

        // Never requested -> do not probe. slIsFeatureSupported on an unrequested feature returns
        // eErrorFeatureMissing, which FromSLResult reads as NotLoaded ("plugin DLL not found") and sends
        // someone hunting for a DLL that is right there.
        if (!m_requested[i])
        {
            m_status[i] = FeatureStatus::NotRequested;
            LOG_INFO("Streamline: %s - not requested by this build", FeatureName(f));
            continue;
        }

        const sl::Result r = slIsFeatureSupported(ToSLFeature(f), adapter);
        m_status[i] = FromSLResult(r);

        // Log success as well as failure: a diagnostic silent on success makes "supported" and "never
        // asked" indistinguishable.
        if (m_status[i] == FeatureStatus::Supported)
            LOG_INFO("Streamline: %s - supported", FeatureName(f));
        else
            // Include the raw result name, not just the coarse category: several codes collapse into
            // "no hardware", and "this GPU cannot" vs "this build is not authorised" decides the fix.
            LOG_INFO("Streamline: %s - UNAVAILABLE (%s) [%s]",
                     FeatureName(f), FeatureStatusName(m_status[i]), ResultName(r));
    }
    Sys::Boot::Mark("streamlineFeatures");   // the NGX feature probes end here
}

bool StreamlineBackendSL::UpgradeSwapChain(IDXGISwapChain** inOutSwapChain)
{
    if (!m_initialized || !inOutSwapChain || !*inOutSwapChain) return false;

    void* iface = static_cast<void*>(*inOutSwapChain);
    const sl::Result r = slUpgradeInterface(&iface);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slUpgradeInterface(swapchain) failed (result=%d) — "
                    "Frame Generation unavailable", static_cast<int>(r));
        m_status[static_cast<uint32_t>(Feature::DLSS_G)] = FeatureStatus::InitFailed;
        return false;
    }
    *inOutSwapChain = static_cast<IDXGISwapChain*>(iface);
    LOG_INFO("Streamline: swap chain upgraded to SL proxy");
    return true;
}

bool StreamlineBackendSL::UpgradeInterface(void** inOutIface)
{
    if (!m_initialized || !inOutIface || !*inOutIface) return false;
    const sl::Result r = slUpgradeInterface(inOutIface);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slUpgradeInterface failed: %s", ResultName(r));
        return false;
    }
    return true;
}

void* StreamlineBackendSL::NewFrameToken(uint32_t frameIndex)
{
    if (!m_initialized) return nullptr;
    sl::FrameToken* token = nullptr;
    const sl::Result r = slGetNewFrameToken(token, &frameIndex);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slGetNewFrameToken failed: %s", ResultName(r));
        m_tokenGame.store(nullptr, std::memory_order_release);
        return nullptr;
    }
    // A null token is a legal, silent state: every wrapper here returns early on one, so a token
    // failure never becomes a crash on the render half.
    m_tokenGame.store(token, std::memory_order_release);
    return token;
}

void* StreamlineBackendSL::GameFrameToken() const
{
    return m_tokenGame.load(std::memory_order_acquire);
}

void StreamlineBackendSL::PublishRenderToken(void* token)
{
    m_tokenRender.store(token, std::memory_order_release);
}

void* StreamlineBackendSL::RenderFrameToken() const
{
    return m_tokenRender.load(std::memory_order_acquire);
}

uint32_t StreamlineBackendSL::FrameIndexOf(void* token) const
{
    if (!token) return UINT32_MAX;
    return static_cast<uint32_t>(*static_cast<sl::FrameToken*>(token));
}

bool StreamlineBackendSL::SetConstants(const FrameConstants& c, void* frameToken)
{
    if (!m_initialized || !frameToken) return false;

    sl::Constants k{};
    CopyMatrix(k.cameraViewToClip, c.cameraViewToClip);
    CopyMatrix(k.clipToCameraView, c.clipToCameraView);
    CopyMatrix(k.clipToPrevClip,   c.clipToPrevClip);
    CopyMatrix(k.prevClipToClip,   c.prevClipToClip);

    k.jitterOffset = { c.jitterOffset.x, c.jitterOffset.y };
    k.mvecScale    = { c.mvecScale.x,    c.mvecScale.y    };
    // Streamline's validator rejects the invalid default ("consts.cameraPinholeOffset should not be
    // left as invalid"), and invalid common constants make DLSS-G refuse to run. The engine uses a
    // centred pinhole, so zero is the true value.
    k.cameraPinholeOffset = { 0.0f, 0.0f };
    k.cameraPos    = { c.cameraPos.x,   c.cameraPos.y,   c.cameraPos.z   };
    k.cameraUp     = { c.cameraUp.x,    c.cameraUp.y,    c.cameraUp.z    };
    k.cameraRight  = { c.cameraRight.x, c.cameraRight.y, c.cameraRight.z };
    k.cameraFwd    = { c.cameraFwd.x,   c.cameraFwd.y,   c.cameraFwd.z   };

    k.cameraNear        = c.cameraNear;
    k.cameraFar         = c.cameraFar;
    k.cameraFOV         = c.cameraFOV;
    k.cameraAspectRatio = c.cameraAspectRatio;

    k.depthInverted          = SLBool(c.depthInverted);
    k.cameraMotionIncluded   = SLBool(c.cameraMotionIncluded);
    k.motionVectors3D        = SLBool(c.motionVectors3D);
    k.orthographicProjection = SLBool(c.orthographicProjection);
    k.reset                  = SLBool(c.reset);

    const sl::Result r = slSetConstants(k, *static_cast<sl::FrameToken*>(frameToken), kViewport);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slSetConstants failed: %s", ResultName(r));
        return false;
    }
    return true;
}

bool StreamlineBackendSL::TagResourcesForFrame(const ResourceTag* tags, uint32_t count,
                                             void* frameToken,
                                             ID3D12GraphicsCommandList* cl)
{
    if (!m_initialized || !frameToken || !tags || count == 0) return false;

    constexpr uint32_t kMaxTags = 16; // color, depth/MV, RR guides and optional overlays
    if (!ENGINE_CHECK(count <= kMaxTags,
                      "Streamline: %u tags exceeds the %u the engine ever needs", count, kMaxTags))
        return false;

    // sl::ResourceTag::resource is a pointer, so the Resource objects must outlive the
    // slSetTagForFrame call below: keep this parallel array, do not collapse it into a temporary
    // inside the loop.
    sl::Resource    slResources[kMaxTags]{};
    sl::ResourceTag slTags[kMaxTags]{};
    for (uint32_t i = 0; i < count; ++i)
    {
        const ResourceTag& t = tags[i];

        // A null native resource says "this buffer does not exist this frame" (paused, loading, UI
        // hidden), and SL accepts it. Never pass a stale pointer instead: that corrupts Frame Generation.
        slResources[i] = sl::Resource{ sl::ResourceType::eTex2d, t.resource, t.state };

        slTags[i].resource  = &slResources[i];
        slTags[i].type      = ToSLBufferType(t.type);
        slTags[i].lifecycle = ToSLLifecycle(t.life);
        // sl::Extent is declared { top, left, width, height }, not left-then-top. All zero means the
        // whole resource, which every current call site passes.
        slTags[i].extent    = { t.top, t.left, t.width, t.height };
    }

    const sl::Result r = slSetTagForFrame(*static_cast<sl::FrameToken*>(frameToken),
                                          kViewport, slTags, count,
                                          static_cast<sl::CommandBuffer*>(cl));
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slSetTagForFrame failed: %s", ResultName(r));
        return false;
    }
    return true;
}

bool StreamlineBackendSL::FreeFeatureResources(Feature f)
{
    std::lock_guard<std::mutex> lock(m_optionsMutex);
    if(!m_initialized || !IsFeatureSupported(f)) return false;
    const auto result=slFreeResources(ToSLFeature(f),kViewport);
    if(result!=sl::Result::eOk)
    { LOG_WARNING("Streamline: slFreeResources(%s) failed: %s",FeatureName(f),ResultName(result)); return false; }
    LOG_INFO("Streamline: released %s viewport resources",FeatureName(f));
    return true;
}

bool StreamlineBackendSL::EvaluateFeature(Feature f, void* frameToken,
                                        ID3D12GraphicsCommandList* cl)
{
    if (!m_initialized || !frameToken || !cl) return false;

    // The viewport is an explicit input: with an empty inputs list Streamline cannot tell which
    // viewport's tags, constants and options this evaluate refers to and returns
    // eErrorMissingInputParameter; the feature then never writes its output, which shows as a black
    // screen.
    const sl::BaseStructure* inputs[] = { &kViewport };

    const sl::Result r = slEvaluateFeature(ToSLFeature(f),
                                           *static_cast<sl::FrameToken*>(frameToken),
                                           inputs,
                                           static_cast<uint32_t>(sizeof(inputs) / sizeof(inputs[0])),
                                           static_cast<sl::CommandBuffer*>(cl));
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slEvaluateFeature(%s) failed: %s",
                    FeatureName(f), ResultName(r));
        return false;
    }
    return true;
}

void* StreamlineBackendSL::GetNativeInterface(void* proxy) const
{
    if (!m_initialized || !proxy) return proxy;
    void* native = nullptr;
    if (slGetNativeInterface(proxy, &native) != sl::Result::eOk || !native)
        return proxy;
    return native;
}

// ==== DLSS Super Resolution ====
namespace
{
    sl::DLSSMode ToSLDLSSMode(DLSSMode m)
    {
        switch (m)
        {
            case DLSSMode::MaxQuality:       return sl::DLSSMode::eMaxQuality;
            case DLSSMode::Balanced:         return sl::DLSSMode::eBalanced;
            case DLSSMode::MaxPerformance:   return sl::DLSSMode::eMaxPerformance;
            case DLSSMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
            case DLSSMode::DLAA:             return sl::DLSSMode::eDLAA;
            case DLSSMode::Off:
            default:                         return sl::DLSSMode::eOff;
        }
    }
}

bool StreamlineBackendSL::GetDLSSOptimalSettings(DLSSMode mode, uint32_t outW, uint32_t outH,
                                               DLSSOptimalSettings& out) const
{
    if (!m_initialized || mode == DLSSMode::Off || outW == 0 || outH == 0) return false;

    sl::DLSSOptions opt{};
    opt.mode         = ToSLDLSSMode(mode);
    opt.outputWidth  = outW;
    opt.outputHeight = outH;

    sl::DLSSOptimalSettings s{};
    const sl::Result r = slDLSSGetOptimalSettings(opt, s);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slDLSSGetOptimalSettings failed: %s", ResultName(r));
        return false;
    }

    out.renderW   = s.optimalRenderWidth;
    out.renderH   = s.optimalRenderHeight;
    out.minW      = s.renderWidthMin;
    out.minH      = s.renderHeightMin;
    out.maxW      = s.renderWidthMax;
    out.maxH      = s.renderHeightMax;
    out.sharpness = s.optimalSharpness;

    // A zero render extent would silently size every render-res target to
    // nothing; refuse rather than propagate it.
    return ENGINE_CHECK(out.renderW > 0 && out.renderH > 0,
                        "Streamline: DLSS returned a zero render extent for %ux%u", outW, outH);
}

bool StreamlineBackendSL::GetDLSSDOptimalSettings(DLSSMode mode, uint32_t outW, uint32_t outH,
                                                DLSSOptimalSettings& out) const
{
    out = DLSSOptimalSettings{};
    if (!m_initialized || mode == DLSSMode::Off || outW == 0 || outH == 0) return false;
    if (!IsFeatureSupported(Feature::DLSS_RR)) return false;

    sl::DLSSDOptions opt{};
    opt.mode         = ToSLDLSSMode(mode);
    opt.outputWidth  = outW;
    opt.outputHeight = outH;

    sl::DLSSDOptimalSettings s{};
    const sl::Result r = slDLSSDGetOptimalSettings(opt, s);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slDLSSDGetOptimalSettings failed: %s", ResultName(r));
        return false;
    }
    out.renderW   = s.optimalRenderWidth;
    out.renderH   = s.optimalRenderHeight;
    out.minW      = s.renderWidthMin;
    out.minH      = s.renderHeightMin;
    out.maxW      = s.renderWidthMax;
    out.maxH      = s.renderHeightMax;
    out.sharpness = s.optimalSharpness;
    return ENGINE_CHECK(out.renderW > 0 && out.renderH > 0,
                        "Streamline: DLSS-RR returned a zero render extent for %ux%u", outW, outH);
}

bool StreamlineBackendSL::GetDLSSDVRAMBytes(uint64_t& out) const
{
    out = 0;
    std::lock_guard<std::mutex> lock(m_optionsMutex);
    if (!m_initialized || !IsFeatureSupported(Feature::DLSS_RR)) return false;
    sl::DLSSDState st{};
    if (slDLSSDGetState(kViewport, st) != sl::Result::eOk) return false;
    out = st.estimatedVRAMUsageInBytes;
    return true;
}

static sl::DLSSPreset ToSLDLSSPreset(DLSSPreset p)
{
    switch (p)
    {
        case DLSSPreset::J: return sl::DLSSPreset::ePresetJ;
        case DLSSPreset::K: return sl::DLSSPreset::ePresetK;
        case DLSSPreset::L: return sl::DLSSPreset::ePresetL;
        case DLSSPreset::M: return sl::DLSSPreset::ePresetM;
        default:            return sl::DLSSPreset::eDefault;
    }
}

bool StreamlineBackendSL::SetDLSSOptions(DLSSMode mode, uint32_t outW, uint32_t outH,
                                       float sharpness, bool useAutoExposure,
                                       DLSSPreset preset)
{
    // One option set per viewport inside the SDK; see m_optionsMutex.
    std::lock_guard<std::mutex> _slOpts(m_optionsMutex);

    if (!m_initialized) return false;

    sl::DLSSOptions opt{};
    opt.mode            = ToSLDLSSMode(mode);
    opt.outputWidth     = outW;
    opt.outputHeight    = outH;

    // Set every per-mode preset field to the same model, not just the one for `mode`: the SDK picks
    // the field by the mode, and the others left on eDefault would make a quality change in the
    // editor silently switch model too. One model across all modes keeps a quality change a pure
    // resolution change.
    const sl::DLSSPreset slPreset = ToSLDLSSPreset(preset);
    opt.dlaaPreset             = slPreset;
    opt.qualityPreset          = slPreset;
    opt.balancedPreset         = slPreset;
    opt.performancePreset      = slPreset;
    opt.ultraPerformancePreset = slPreset;
    opt.ultraQualityPreset     = slPreset;
    // DLSSOptions::sharpness is deprecated in this SDK (DLSS 4 sharpens internally per preset), and
    // writing it only earns a warning. The value is accepted and ignored so the engine API need not
    // change if it returns.
    (void)sharpness;
    // false: the caller tags the engine's exposure (DLSSPass, pass.upscaler.engineExposure); the FSR2 path
    // makes the same choice with FFX_UPSCALE_ENABLE_AUTO_EXPOSURE.
    opt.useAutoExposure = useAutoExposure ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    opt.colorBuffersHDR = sl::Boolean::eTrue;   // engine scene colour is linear FP16

    const sl::Result r = slDLSSSetOptions(kViewport, opt);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slDLSSSetOptions failed: %s", ResultName(r));
        return false;
    }
    return true;
}

// ==== DLSS Ray Reconstruction options ====
// Guide production and evaluation belong to the caller.
bool StreamlineBackendSL::SetDLSSDOptions(DLSSMode mode,uint32_t outW,uint32_t outH,
    const DirectX::XMFLOAT4X4& worldToView,const DirectX::XMFLOAT4X4& viewToWorld,DLSSRRModel model,float exposureScale,float preExposure)
{
    std::lock_guard<std::mutex> lock(m_optionsMutex);
    if(!m_initialized || !IsFeatureSupported(Feature::DLSS_RR)) return false;
    if(model!=DLSSRRModel::D && model!=DLSSRRModel::E && model!=DLSSRRModel::F) return false;
    if(!std::isfinite(exposureScale) || exposureScale<=0.f) return false;
    if(mode!=DLSSMode::Off && (outW==0 || outH==0)) return false;
    for(uint32_t row=0;row<4;++row) for(uint32_t col=0;col<4;++col)
        if(!std::isfinite(worldToView.m[row][col]) || !std::isfinite(viewToWorld.m[row][col])) return false;
    sl::DLSSDOptions opt{};
    opt.mode=ToSLDLSSMode(mode); opt.outputWidth=outW; opt.outputHeight=outH;
    opt.colorBuffersHDR=sl::Boolean::eTrue;
    opt.normalRoughnessMode=sl::DLSSDNormalRoughnessMode::ePacked;
    if(!std::isfinite(preExposure) || preExposure<=0.f) return false;
    opt.exposureScale=exposureScale;
    opt.preExposure=preExposure;
    const auto preset=model==DLSSRRModel::F?sl::DLSSDPreset::ePresetF
                     :(model==DLSSRRModel::E?sl::DLSSDPreset::ePresetE:sl::DLSSDPreset::ePresetD);
    opt.dlaaPreset=opt.qualityPreset=opt.balancedPreset=opt.performancePreset=
        opt.ultraPerformancePreset=opt.ultraQualityPreset=preset;
    static_assert(sizeof(opt.worldToCameraView)==sizeof(worldToView));
    std::memcpy(&opt.worldToCameraView,&worldToView,sizeof(worldToView));
    std::memcpy(&opt.cameraViewToWorld,&viewToWorld,sizeof(viewToWorld));
    const sl::Result result=slDLSSDSetOptions(kViewport,opt);
    if(result!=sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slDLSSDSetOptions failed: %s",ResultName(result));
        return false;
    }
    return true;
}

// ==== DLSS Frame Generation ====
bool StreamlineBackendSL::SetFrameGenOptions(FrameGenMode mode, uint32_t numFramesToGenerate,
                                           uint32_t hudLessFormat, uint32_t uiFormat)
{
    // One option set per viewport inside the SDK; see m_optionsMutex.
    std::lock_guard<std::mutex> _slOpts(m_optionsMutex);

    if (!m_initialized) return false;

    sl::DLSSGOptions opt{};
    switch (mode)
    {
        case FrameGenMode::On:      opt.mode = sl::DLSSGMode::eOn;      break;
        case FrameGenMode::Auto:    opt.mode = sl::DLSSGMode::eAuto;    break;
        case FrameGenMode::Dynamic: opt.mode = sl::DLSSGMode::eDynamic; break;
        case FrameGenMode::Off:
        default:                    opt.mode = sl::DLSSGMode::eOff;     break;
    }
    opt.numFramesToGenerate = numFramesToGenerate;

    // Tell DLSS-G that HUD-less colour comes separately; without this it does not allocate the path
    // that interpolates the scene apart from the UI, and the HUD-less tag is accepted but unused.
    opt.enableUserInterfaceRecomposition = sl::Boolean::eTrue;

    // Declare the HUD-less / UI formats when they differ from the back buffer's: redundant in SDR
    // (all R8G8B8A8_UNORM); in HDR10 the engine composites in FP16 onto an RGB10A2 back buffer, and
    // declaring it makes the mismatch legal instead of an error.
    opt.hudLessBufferFormat = hudLessFormat;
    opt.uiBufferFormat      = uiFormat;

    // Ask for the VRAM figure: without this flag DLSSGState::estimatedVRAMUsageInBytes stays 0, as if
    // Frame Generation (a few hundred MB at 1080p, more at 4K) cost nothing.
    opt.flags = sl::DLSSGFlags::eRequestVRAMEstimate;

    // Surface the DXGI/API errors DLSS-G hits internally: Frame Generation can fail with
    // eDLSSGStatusOk and no other diagnostic.
    opt.onErrorCallback = [](const sl::APIError& e)
    {
        LOG_ERROR("Streamline: DLSS-G API error hr=0x%08X", static_cast<unsigned>(e.hres));
    };

    const sl::Result r = slDLSSGSetOptions(kViewport, opt);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slDLSSGSetOptions failed: %s", ResultName(r));
        return false;
    }
    m_fgMode = mode;
    return true;
}

bool StreamlineBackendSL::GetFrameGenState(FrameGenState& out) const
{
    // One option set per viewport inside the SDK; see m_optionsMutex.
    std::lock_guard<std::mutex> _slOpts(m_optionsMutex);

    out = FrameGenState{};
    if (!m_initialized) return false;

    sl::DLSSGState st{};
    // The options parameter is an input ("the state for these options"); nullptr asks about the
    // configured ones. The active mode comes from m_fgMode.
    const sl::Result r = slDLSSGGetState(kViewport, st, nullptr);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slDLSSGGetState failed: %s", ResultName(r));
        return false;
    }

    out.statusOk               = (st.status == sl::DLSSGStatus::eOk);
    out.framesPresentedSincePoll = st.numFramesActuallyPresented;
    out.numFramesToGenerateMax = st.numFramesToGenerateMax;
    out.estimatedVRAMBytes     = st.estimatedVRAMUsageInBytes;
    out.dynamicMFGSupported    = (st.bIsDynamicMFGSupported == sl::Boolean::eTrue);
    out.vsyncSupported         = (st.bIsVsyncSupportAvailable == sl::Boolean::eTrue);
    out.enabled                = out.statusOk && m_fgMode != FrameGenMode::Off;

    if (!out.statusOk)
    {
        // DLSSGStatus is a bitfield (each failure 1<<n) and several bits can be set at once; a switch on
        // the whole value would report "unspecified" exactly when two problems coincide. Report the first
        // set bit, most actionable first.
        const uint32_t s = static_cast<uint32_t>(st.status);
        auto has = [s](sl::DLSSGStatus bit) {
            return (s & static_cast<uint32_t>(bit)) != 0;
        };

        if (has(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime))
            out.statusText = "Reflex is not active (Frame Generation requires it)";
        else if (has(sl::DLSSGStatus::eFailHDRFormatNotSupported))
            out.statusText = "back-buffer format unsupported (use SDR or HDR10, not scRGB FP16)";
        else if (has(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled))
            out.statusText = "swap chain not driven through the Streamline proxy";
        else if (has(sl::DLSSGStatus::eFailCommonConstantsInvalid))
            out.statusText = "per-frame constants invalid (check matrices and frame index)";
        else if (has(sl::DLSSGStatus::eFailResolutionTooLow))
            out.statusText = "render resolution too low for Frame Generation";
        else
            out.statusText = "Frame Generation reported an unspecified failure";
    }
    return true;
}

// ==== Reflex + PCL ====
bool StreamlineBackendSL::SetReflexOptions(ReflexMode mode, uint32_t frameLimitUs)
{
    // One option set per viewport inside the SDK; see m_optionsMutex.
    std::lock_guard<std::mutex> _slOpts(m_optionsMutex);

    if (!m_initialized) return false;

    sl::ReflexOptions opt{};
    switch (mode)
    {
        case ReflexMode::LowLatency:          opt.mode = sl::ReflexMode::eLowLatency;          break;
        case ReflexMode::LowLatencyWithBoost: opt.mode = sl::ReflexMode::eLowLatencyWithBoost; break;
        case ReflexMode::Off:
        default:                              opt.mode = sl::ReflexMode::eOff;                 break;
    }
    opt.frameLimitUs = frameLimitUs;

    const sl::Result r = slReflexSetOptions(opt);
    if (r != sl::Result::eOk)
    {
        LOG_WARNING("Streamline: slReflexSetOptions failed: %s", ResultName(r));
        return false;
    }
    m_reflexMode = mode;
    return true;
}

bool StreamlineBackendSL::ReflexSleep(void* frameToken)
{
    if (!m_initialized || !frameToken) return false;
    const sl::Result r = slReflexSleep(*static_cast<sl::FrameToken*>(frameToken));
    return r == sl::Result::eOk;
}

bool StreamlineBackendSL::SetLatencyMarker(LatencyMarker marker, void* frameToken)
{
    if (!m_initialized || !frameToken) return false;

    sl::PCLMarker m;
    switch (marker)
    {
        case LatencyMarker::SimulationStart:   m = sl::PCLMarker::eSimulationStart;   break;
        case LatencyMarker::SimulationEnd:     m = sl::PCLMarker::eSimulationEnd;     break;
        case LatencyMarker::RenderSubmitStart: m = sl::PCLMarker::eRenderSubmitStart; break;
        case LatencyMarker::RenderSubmitEnd:   m = sl::PCLMarker::eRenderSubmitEnd;   break;
        case LatencyMarker::PresentStart:      m = sl::PCLMarker::ePresentStart;      break;
        case LatencyMarker::PresentEnd:        m = sl::PCLMarker::ePresentEnd;        break;
        default:                               return false;
    }

    const sl::Result r = slPCLSetMarker(m, *static_cast<sl::FrameToken*>(frameToken));
    if (r != sl::Result::eOk &&
        !m_loggedMarkerFailure.exchange(true, std::memory_order_relaxed))
    {
        // Once, not per frame (markers fire six times a frame), but not silent: a broken marker chain
        // costs nothing visible at first and later makes Frame Generation reject every frame, with the
        // failure pointing at DLSS-G instead of the markers.
        LOG_WARNING("Streamline: slPCLSetMarker failed: %s "
                    "(further marker failures suppressed) - Reflex latency data and, "
                    "later, Frame Generation pacing will be wrong", ResultName(r));
    }
    return r == sl::Result::eOk;
}

// Installed by StreamlineModule::OnProcessStart.
StreamlineBackend& StreamlineBackendSDK()
{
    static StreamlineBackendSL s_backend;
    return s_backend;
}

} // namespace SL

#endif  // ENGINE_WITH_STREAMLINE
