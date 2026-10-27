// StreamlineModule: NVIDIA Streamline (DLSS Super Resolution, Ray Reconstruction, Frame Generation,
// Reflex) as an engine module (src/Modules/Streamline.module.json). Its backend is the SDK half of
// SL::StreamlineBackend; without this module the exe links no sl.interposer.lib, imports no
// sl.interposer.dll and the core's dormant backend reports every feature NoSDK.

#include "Module/EngineModule.h"

#include "Graphics/StreamlineBackend.h"

#if ENGINE_WITH_STREAMLINE
namespace SL
{
    StreamlineBackend& StreamlineBackendSDK();   // StreamlineBackendSL.cpp
}
#endif

class StreamlineModule final : public Module::IEngineModule
{
public:
    // Before App constructs GraphicsDX12, whose LoadPipeline calls Init ahead of the DXGI factory.
    void OnProcessStart() override
    {
#if ENGINE_WITH_STREAMLINE
        SL::StreamlineBackend::Install(&SL::StreamlineBackendSDK());
#endif
    }
};

ENGINE_MODULE(StreamlineModule)
