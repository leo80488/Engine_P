// Renderer/Features/StreamUploadProbeFeature.cpp - the reader of the streaming texture
// upload probes (Harness/StreamUploadProbe.h, and a module's through Harness::UploadReadbacks).
//
// A graph pass in the GBuffer stage, the first stage of the main graph and so the
// earliest reader a frame has after the StreamUpload list. It copies the probes'
// stream targets into READBACK buffers; each probe compares them with what it expects
// once the slot retires. If the queued copies ran after this pass (the reverse arm,
// -streamupload_probe_primary, records them into the primary list), the readback holds
// the previous frame's data and the stream probe counts a mismatch. A module's probe (the Video
// module's video probe) records its own reads (its record is a no-op until it has one).
//
// Present in every build so the generated feature list is the same everywhere; the
// pass exists only in a harness build with a probe requested (source gate 8: a pass
// the Renderer does not own lives in a feature).

#include "Renderer/RenderFeature.h"

#include "Graphics/IGraphicsDevice.h"
#if defined(WITH_HARNESS) && WITH_HARNESS
#include "Harness/StreamUploadProbe.h"
#endif

namespace
{
#if defined(WITH_HARNESS) && WITH_HARNESS
class StreamUploadProbePass final : public RG::RenderPass
{
public:
    const char* GetName() const override { return "StreamUploadProbe"; }
    // No graph texture: the probes' targets are the device's (stream targets
    // are never graph-tracked), and the graph culls nothing.
    void Setup(RG::RenderGraphBuilder&) override {}
    void Init(IGraphicsDevice&) override {}
    RHI::CommandList Execute(RHI::CommandList cl) override
    {
        if (Harness::StreamUploadProbe::Requested())
            Harness::StreamUploadProbe::Get().RecordReadback(cl.GetDevice(), cl);
        for (const Harness::UploadReadback& r : Harness::UploadReadbacks())
            if (r.requested()) r.record(cl.GetDevice(), cl);
        return cl;
    }
};
#endif

class StreamUploadProbeFeature final : public IRenderFeature
{
public:
    const char* Name() const override { return "StreamUploadProbe"; }
    uint32_t    Stages() const override { return StageBit(RG::Stage::GBuffer); }

    void AddPasses(RG::Stage, FeatureCompileContext& ctx) override
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        bool requested = Harness::StreamUploadProbe::Requested();
        for (const Harness::UploadReadback& r : Harness::UploadReadbacks())
            requested = requested || r.requested();
        if (requested)
            ctx.AddPass(std::make_unique<StreamUploadProbePass>());
#else
        (void)ctx;
#endif
    }
};
}

REGISTER_RENDER_FEATURE(StreamUploadProbeFeature)
