#include "Graphics/StreamlineBackend.h"
#include "System/Log.h"
#include "System/Check.h"
#include "System/BootTimeline.h"

#include <cstring>
#include <cmath>

// StreamlineBackend.cpp - the core half of StreamlineBackend.h: the name tables, the status
// accessors, the installed backend, and the dormant behaviour every virtual defaults to (no
// Streamline module in this exe, or an SDK-less build: nothing loads, every feature reports NoSDK).
// The Streamline module's StreamlineBackendSL (src/Streamline) is the SDK half.

namespace SL
{

// ==== Name tables ====
// Always compiled: the editor and the boot log use them, also in a build with no SDK.
const char* FeatureName(Feature f)
{
    switch (f)
    {
        case Feature::DLSS:   return "DLSS Super Resolution";
        case Feature::DLSS_G: return "DLSS Frame Generation";
        case Feature::DLSS_RR: return "DLSS Ray Reconstruction";
        case Feature::Reflex: return "Reflex";
        case Feature::PCL:    return "PC Latency markers";
        default:              return "<unknown>";
    }
}

const char* FeatureStatusName(FeatureStatus s)
{
    switch (s)
    {
        case FeatureStatus::Supported:      return "supported";
        case FeatureStatus::NoSDK:          return "Streamline SDK not compiled in";
        case FeatureStatus::InitFailed:     return "Streamline failed to initialise";
        case FeatureStatus::NoHardware:     return "GPU does not support this feature";
        case FeatureStatus::DriverTooOld:   return "graphics driver is too old";
        case FeatureStatus::OSUnsupported:  return "OS requirement not met (needs Win10 20H1+ and hardware GPU scheduling)";
        case FeatureStatus::NotLoaded:      return "feature plugin DLL not found";
        case FeatureStatus::Denied:         return "blocked by driver or OS policy";
        case FeatureStatus::NotRequested:   return "not loaded by this build (see Init's request list)";
        case FeatureStatus::Unknown:
        default:                            return "not yet queried";
    }
}

const char* DLSSModeName(DLSSMode m)
{
    switch (m)
    {
        case DLSSMode::DLAA:             return "DLAA (native)";
        case DLSSMode::MaxQuality:       return "Quality";
        case DLSSMode::Balanced:         return "Balanced";
        case DLSSMode::MaxPerformance:   return "Performance";
        case DLSSMode::UltraPerformance: return "Ultra Performance";
        case DLSSMode::Off:
        default:                         return "Off";
    }
}

const char* DLSSPresetName(DLSSPreset p)
{
    switch (p)
    {
        case DLSSPreset::J: return "J (transformer, less ghosting, more flicker)";
        case DLSSPreset::K: return "K (SDK default for DLAA/Quality/Balanced)";
        case DLSSPreset::L: return "L (sharper, more stable, costs more)";
        case DLSSPreset::M: return "M (~L quality, ~K speed)";
        case DLSSPreset::Default:
        default:            return "Driver default (per mode; can change via OTA)";
    }
}

namespace
{
    // A module's backend (Install), else the dormant one.
    StreamlineBackend*& Installed()
    {
        static StreamlineBackend* s_installed = nullptr;
        return s_installed;
    }
}

StreamlineBackend& StreamlineBackend::Get()
{
    static StreamlineBackend s_dormant;
    StreamlineBackend* installed = Installed();
    return installed ? *installed : s_dormant;
}

void StreamlineBackend::Install(StreamlineBackend* backend)
{
    Installed() = backend;
}

uint32_t StreamlineBackend::TokenFrameIndex(void* token)
{
    return Get().FrameIndexOf(token);
}

bool StreamlineBackend::IsFeatureSupported(Feature f) const
{
    const uint32_t i = static_cast<uint32_t>(f);
    if (i >= static_cast<uint32_t>(Feature::Count)) return false;
    return m_status[i] == FeatureStatus::Supported;
}

FeatureStatus StreamlineBackend::GetFeatureStatus(Feature f) const
{
    const uint32_t i = static_cast<uint32_t>(f);
    if (i >= static_cast<uint32_t>(Feature::Count)) return FeatureStatus::Unknown;
    return m_status[i];
}

const char* StreamlineBackend::GetFeatureStatusText(Feature f) const
{
    return FeatureStatusName(GetFeatureStatus(f));
}

// ==== Dormant behaviour ====
// No SDK behind it: every feature reports NoSDK (Init), nothing is created.

bool StreamlineBackend::Init(const InitDesc&)
{
    for (auto& s : m_status) s = FeatureStatus::NoSDK;
    // INFO, not WARNING: building without the SDK is a supported configuration. Still logged, so
    // "why is DLSS missing" is answerable from DX12Log.txt alone.
    LOG_INFO("Streamline: SDK not compiled in (no Streamline module in this exe, or ENGINE_WITH_STREAMLINE=0) — "
             "DLSS and Frame Generation disabled. See external/Streamline/PLACE_SDK_HERE.md");
    return false;
}

void  StreamlineBackend::Shutdown()                                      {}
void  StreamlineBackend::SetD3DDevice(ID3D12Device*)                     {}
bool  StreamlineBackend::UpgradeSwapChain(IDXGISwapChain**)              { return false; }
bool  StreamlineBackend::UpgradeInterface(void**)                        { return false; }
void* StreamlineBackend::NewFrameToken(uint32_t)                         { return nullptr; }
void* StreamlineBackend::GameFrameToken() const                          { return nullptr; }
void  StreamlineBackend::PublishRenderToken(void*)                       {}
void* StreamlineBackend::RenderFrameToken() const                        { return nullptr; }
uint32_t StreamlineBackend::FrameIndexOf(void*) const                  { return UINT32_MAX; }
bool  StreamlineBackend::SetConstants(const FrameConstants&, void*)      { return false; }
bool  StreamlineBackend::TagResourcesForFrame(const ResourceTag*, uint32_t,
                                              void*, ID3D12GraphicsCommandList*) { return false; }
bool  StreamlineBackend::EvaluateFeature(Feature, void*, ID3D12GraphicsCommandList*) { return false; }
bool  StreamlineBackend::FreeFeatureResources(Feature) { return false; }
void* StreamlineBackend::GetNativeInterface(void* proxy) const           { return proxy; }

bool StreamlineBackend::GetDLSSOptimalSettings(DLSSMode, uint32_t, uint32_t,
                                               DLSSOptimalSettings& out) const
{
    out = DLSSOptimalSettings{};
    return false;
}
bool StreamlineBackend::GetDLSSDOptimalSettings(DLSSMode, uint32_t, uint32_t,
                                                DLSSOptimalSettings& out) const
{
    out = DLSSOptimalSettings{};
    return false;
}
bool StreamlineBackend::GetDLSSDVRAMBytes(uint64_t& out) const { out = 0; return false; }
bool StreamlineBackend::SetDLSSOptions(DLSSMode, uint32_t, uint32_t, float, bool, DLSSPreset) {
    // One option set per viewport inside the SDK; see m_optionsMutex.
    std::lock_guard<std::mutex> _slOpts(m_optionsMutex);
 return false; }
bool StreamlineBackend::SetDLSSDOptions(DLSSMode,uint32_t,uint32_t,
    const DirectX::XMFLOAT4X4&,const DirectX::XMFLOAT4X4&,DLSSRRModel,float,float) { return false; }

bool StreamlineBackend::SetFrameGenOptions(FrameGenMode, uint32_t, uint32_t, uint32_t) {
    // One option set per viewport inside the SDK; see m_optionsMutex.
    std::lock_guard<std::mutex> _slOpts(m_optionsMutex);
 return false; }
bool StreamlineBackend::GetFrameGenState(FrameGenState& out) const
{
    // One option set per viewport inside the SDK; see m_optionsMutex.
    std::lock_guard<std::mutex> _slOpts(m_optionsMutex);

    out = FrameGenState{};
    out.statusText = "Streamline SDK not compiled in";
    return false;
}


bool StreamlineBackend::SetReflexOptions(ReflexMode, uint32_t)      {
    // One option set per viewport inside the SDK; see m_optionsMutex.
    std::lock_guard<std::mutex> _slOpts(m_optionsMutex);
 return false; }

bool StreamlineBackend::ReflexSleep(void*)                          { return false; }
bool StreamlineBackend::SetLatencyMarker(LatencyMarker, void*)      { return false; }


} // namespace SL
