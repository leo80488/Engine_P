// VideoModule: video playback as an engine module (src/Modules/Video.module.json) - the
// VideoComponent's serializer, its decode system, the screen overlay and world-space quads (a render
// feature, Video/VideoFeature.h) and, in a harness build, -video_probe. Video::OpenMp4
// (Video/Mp4FrameSource.h) plays a file through FFmpeg.

#include "Module/EngineModule.h"

#include "ECS/ECS.h"
#include "ECS/SystemRegistry.h"
#include "ECS/VideoComponent.h"
#include "ECS/VideoSystem.h"
#include "Renderer/RenderFeature.h"
#include "Resource/ComponentKV.h"
#include "Resource/ComponentSerializers.h"
#include "Video/VideoFeature.h"
#if defined(WITH_HARNESS) && WITH_HARNESS
#include "Harness/StreamUploadProbe.h"   // the feature pass that reads the probe's texture back
#include "Harness/VideoUploadProbe.h"
#endif

#include <cstdio>
#include <sstream>
#include <typeindex>

namespace
{
    using namespace ComponentKV;

    // Stream config + playback knobs + surface placement: everything the inspector exposes, plus the
    // stream fields it does not (profile / bitDepth / dpbSlotCount), equally authoring-time: without
    // them a 10-bit HEVC surface would reload as 8-bit H.264.
    // Not saved: decodedFrameSource (a shared_ptr the game code installs), dpb[], currentDpbSlot,
    // clockSeconds, lastDecodedPts, framesDecoded: GPU handles or the running stream clock.
    void RegisterVideoSerializer(ComponentSerializerRegistry& reg)
    {
        reg.Register(std::type_index(typeid(VideoComponent)), {
            "Video",
            [](World& w, Entity e) { return w.GetComponent<VideoComponent>(e) != nullptr; },
            [](World& w, Entity e, std::ostringstream& ss) {
                const auto* v = w.GetComponent<VideoComponent>(e);
                if (!v) return;
                char buf[448];
                snprintf(buf, sizeof(buf),
                    "  Video: codec=%u profile=%u width=%u height=%u bitDepth=%u"
                    " dpbSlots=%u state=%u loop=%u playRate=%.4f"
                    " worldSpace=%u worldWidth=%.4f worldHeight=%.4f"
                    " uMin=%.4f vMin=%.4f uMax=%.4f vMax=%.4f"
                    " alpha=%.4f colorSpace=%u\n",
                    static_cast<uint32_t>(v->codec), static_cast<uint32_t>(v->profile),
                    v->width, v->height, v->bitDepth, v->dpbSlotCount,
                    static_cast<uint32_t>(v->state), v->loop ? 1u : 0u, v->playRate,
                    v->worldSpace ? 1u : 0u, v->worldWidth, v->worldHeight,
                    v->rectUMin, v->rectVMin, v->rectUMax, v->rectVMax,
                    v->renderAlpha, v->colorSpace);
                ss << buf;
            },
            [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
                VideoComponent v;
                v.codec        = static_cast<RHI::VideoCodec>(
                                     GetI(kv, "codec",   static_cast<int>(v.codec)));
                v.profile      = static_cast<RHI::VideoProfile>(
                                     GetI(kv, "profile", static_cast<int>(v.profile)));
                v.width        = GetU(kv, "width",     v.width);
                v.height       = GetU(kv, "height",    v.height);
                v.bitDepth     = GetU(kv, "bitDepth",  v.bitDepth);
                v.dpbSlotCount = GetU(kv, "dpbSlots",  v.dpbSlotCount);
                v.state        = static_cast<VideoPlaybackState>(
                                     GetI(kv, "state",  static_cast<int>(v.state)));
                v.loop         = GetI(kv, "loop", 0) != 0;
                v.playRate     = GetF(kv, "playRate", v.playRate);
                v.worldSpace   = GetI(kv, "worldSpace", 0) != 0;
                v.worldWidth   = GetF(kv, "worldWidth",  v.worldWidth);
                v.worldHeight  = GetF(kv, "worldHeight", v.worldHeight);
                v.rectUMin     = GetF(kv, "uMin", v.rectUMin);
                v.rectVMin     = GetF(kv, "vMin", v.rectVMin);
                v.rectUMax     = GetF(kv, "uMax", v.rectUMax);
                v.rectVMax     = GetF(kv, "vMax", v.rectVMax);
                v.renderAlpha  = GetF(kv, "alpha", v.renderAlpha);
                v.colorSpace   = GetU(kv, "colorSpace", v.colorSpace);
                // Frame sources are re-installed by game code; without one the
                // system just leaves dpb[] alone, so a saved Playing state is inert
                // rather than broken.
                w.AddComponent<VideoComponent>(e, std::move(v));
            }
        });
    }
}

class VideoModule final : public Module::IEngineModule
{
public:
    void RegisterTypes(ComponentSerializerRegistry& components) override
    {
        RegisterVideoSerializer(components);
    }

    // Before the renderer compiles: the probe's readback decides whether the probe pass exists.
    void Initialize(Module::Context&) override
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        Harness::RegisterUploadReadback({
            [] { return Harness::VideoUploadProbe::Requested(); },
            [](IGraphicsDevice& gfx, RHI::CommandList cl) { Harness::VideoUploadProbe::Get().RecordReadback(gfx, cl); } });
#endif
    }

    void RegisterRenderFeatures(RenderFeatureRegistry& features) override
    {
        features.Add(CreateVideoFeature());
    }

    // VideoSystem runs early (GameplayPreLogic): each frame source decodes and queues its frame's copy
    // into the component's texture before the renderer extracts. -video_probe plays a generated clip
    // through Mp4FrameSource and checks the video texture.
    void RegisterSystems(Module::Context& ctx) override
    {
        ctx.systems->Add<VideoSystem>(*ctx.gfx);
#if defined(WITH_HARNESS) && WITH_HARNESS
        if (Harness::VideoUploadProbe::Requested())
            ctx.systems->Add<Harness::VideoUploadProbeSystem>(*ctx.gfx);
#endif
    }
};

ENGINE_MODULE(VideoModule)
