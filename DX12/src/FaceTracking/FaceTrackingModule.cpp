// FaceTrackingModule: face tracking as an engine module (src/Modules/FaceTracking.module.json).
// Everything the core knew of face tracking registers here: the component's serializer, its
// Entity.set row and runtime carry, the .ifacemap hot reload, the animation chain's two stages,
// the head handoff, FaceTracking.* in Lua and the service's lifecycle.

#include "Module/EngineModule.h"

#include "Anim/AnimChainHook.h"
#include "Diagnostics/Profiler.h"
#include "ECS/AnimationComponents.h"          // SkeletonComponent: the binding's carrier
#include "ECS/FaceTrackingComponent.h"
#include "ECS/FaceTrackingSystem.h"
#include "FaceTracking/FaceTrackingService.h"
#include "FaceTracking/LuaFaceTrackingBindings.h"
#include "Renderer/Renderer.h"
#include "Resource/ComponentKV.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"
#include "Scripting/LuaEntityBindings.h"
#include "Settings/SettingBridge.h"
#include "System/HotReloadService.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <sstream>
#include <typeindex>

namespace
{
    using namespace ComponentKV;

    // The authored fields only: the compiled mapping, the filters, the authority and the cursor belong
    // to FaceTrackingSystem and are rebuilt after load. `profile` is a .ifacemap path, so AssetRefs
    // stamps a $g_profile guid beside it and the cook and the asset catalog see the reference.
    void RegisterFaceTrackingSerializer(ComponentSerializerRegistry& reg)
    {
        reg.Register(std::type_index(typeid(FaceTrackingComponent)), {
            "FaceTracking",
            [](World& w, Entity e) { return w.GetComponent<FaceTrackingComponent>(e) != nullptr; },
            [](World& w, Entity e, std::ostringstream& ss) {
                const auto* ft = w.GetComponent<FaceTrackingComponent>(e);
                if (!ft) return;
                char buf[512];
                snprintf(buf, sizeof(buf),
                    "  FaceTracking: enabled=%u source=%s wMorph=%.4f wHead=%.4f wEyes=%.4f mirror=%u"
                    " hold=%.4f fadeOut=%.4f fadeIn=%.4f suppressBlink=%u handoff=%u",
                    ft->enabled ? 1u : 0u, PercentEncode(ft->source).c_str(),
                    ft->weightMorph, ft->weightHead, ft->weightEyes, ft->mirror ? 1u : 0u,
                    ft->holdSec, ft->fadeOutSec, ft->fadeInSec,
                    ft->suppressAutoBlink ? 1u : 0u, ft->headTrackHandoff ? 1u : 0u);
                ss << buf;
                if (!ft->profile.empty())
                    ss << " profile=" << PercentEncode(Sys::RelativizeAssetPath(ft->profile));
                ss << '\n';
            },
            [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
                FaceTrackingComponent ft;
                ft.enabled           = GetI(kv, "enabled", 1) != 0;
                ft.source            = GetS(kv, "source", ft.source);
                ft.profile           = Sys::RelativizeAssetPath(GetS(kv, "profile", ""));
                ft.weightMorph       = SanitizeWeight01(GetF(kv, "wMorph", ft.weightMorph));
                ft.weightHead        = SanitizeWeight01(GetF(kv, "wHead", ft.weightHead));
                ft.weightEyes        = SanitizeWeight01(GetF(kv, "wEyes", ft.weightEyes));
                ft.mirror            = GetI(kv, "mirror", 1) != 0;
                ft.holdSec           = (std::max)(0.0f, GetF(kv, "hold", ft.holdSec));
                ft.fadeOutSec        = (std::max)(0.0f, GetF(kv, "fadeOut", ft.fadeOutSec));
                ft.fadeInSec         = (std::max)(0.0f, GetF(kv, "fadeIn", ft.fadeInSec));
                ft.suppressAutoBlink = GetI(kv, "suppressBlink", 1) != 0;
                ft.headTrackHandoff  = GetI(kv, "handoff", 1) != 0;
                w.AddComponent<FaceTrackingComponent>(e, ft);
            }
        });
    }

    // The binding (the compiled mapping, the filters, the authority, the cursor) and this
    // frame's head authority survive, so an Inspector edit or Entity.set does not drop the face
    // to Lost and fade it back in. FaceTrackingSystem compares the binding with the new authored
    // fields itself: a changed source or mirror re-reads the source's history, a changed profile
    // recompiles.
    ComponentWrite::RuntimeState CaptureFaceTracking(World& w, Entity e)
    {
        const auto* ft = w.GetComponent<FaceTrackingComponent>(e);
        if (!ft) return {};
        auto rt = ft->rt;
        const float headAuthority = ft->headAuthority;
        return ComponentWrite::RuntimeState([rt, headAuthority](World& world, Entity ent) {
            if (auto* now = world.GetComponent<FaceTrackingComponent>(ent))
            {
                now->rt = rt;
                now->headAuthority = headAuthority;
            }
        });
    }

    // FaceTrackingSystem rereads every authored field each frame (a new source or mirror
    // re-reads the source's history, a new profile recompiles); the capture above carries the
    // binding across the rewrite.
    void FaceTrackingWarn(World& w, Entity e, std::vector<std::string>& warnings)
    {
        if (!w.GetComponent<SkeletonComponent>(e))
            warnings.push_back("not on the skeleton entity: face tracking does nothing here (FaceTracking.Attach finds it)");
        if (const auto* ft = w.GetComponent<FaceTrackingComponent>(e))
        {
            FaceTracking::SourceStatus st;
            if (!FaceTracking::Service::Get().Status(ft->source, st))
                warnings.push_back("no source named '" + ft->source + "' is open: the face stays as animated");
        }
    }

    // HeadTrack gives way by this: FaceTrackingSystem::Resolve set it earlier this frame.
    float FaceHeadAuthority(World& w, Entity e)
    {
        const auto* ft = w.GetComponent<FaceTrackingComponent>(e);
        return ft ? ft->headAuthority : 0.0f;
    }

    // A face mapping (or a built-in one it bases on): every face tracking binding
    // reloads and recompiles its mapping on its next frame - a basePreset edit
    // reaches the mappings built on it too.
    void OnFaceMapChanged(const std::string& path)
    {
        FaceTrackingSystem::InvalidateProfiles();
        LOG_INFO("[FACETRACK] mapping changed on disk (%s): every binding recompiles", path.c_str());
    }

    class FaceTrackingChainHook final : public IAnimChainHook
    {
    public:
        explicit FaceTrackingChainHook(FaceTrackingSystem& system) : m_system(system) {}

        // Once per frame before anything reads a face source: Resolve advances the sources'
        // clocks by dt (0 in Edit mode: takes pause), drains their rings into the per-source
        // history (Service::Pump) and writes each binding's Tracked morph list.
        void BeforeFacial(World& world, float dt, const std::unordered_set<Entity>* activeSet) override
        {
            ZoneScopedN("Anim.FaceTrack"); PROF_SCOPE("Anim.FaceTrack");
            m_system.Resolve(world, dt, activeSet);
        }

        void AfterHeadTrack(World& world, const std::unordered_set<Entity>* activeSet) override
        {
            ZoneScopedN("Anim.FaceTrackPose"); PROF_SCOPE("Anim.FaceTrackPose");
            m_system.ApplyPose(world, activeSet);
        }

    private:
        FaceTrackingSystem& m_system;
    };
}

class FaceTrackingModule final : public Module::IEngineModule
{
public:
    void RegisterTypes(ComponentSerializerRegistry& components) override
    {
        RegisterFaceTrackingSerializer(components);
        ComponentWrite::RegisterCapture(std::type_index(typeid(FaceTrackingComponent)), &CaptureFaceTracking);
        RegisterLuaEntityWritable("FaceTracking", &FaceTrackingWarn);
    }

    // Face tracking sources live outside every World; the service reads its own
    // -facetrack_* flags and opens -facetrack_replay's take.
    void Initialize(Module::Context&) override
    {
        FaceTracking::Service::Get().Initialize();
        HotReload::RegisterAssetHandler(".ifacemap", &OnFaceMapChanged);
        // The player's camera permission (settings.json faceTracking.camera): the service
        // refuses to list or open a camera while it is off.
        Settings::RegisterBoolBridge("faceTracking.camera", {
            [](bool allowed) { FaceTracking::Service::Get().SetCameraAllowed(allowed); },
            [] { return FaceTracking::Service::Get().CameraAllowed(); } });
    }

    // The editor preview's texture is created once the device exists; Service::Pump uploads
    // into it. The bindings' state lives on FaceTrackingComponent::rt, so the system holds no
    // per-world map and a world clear needs nothing here.
    void OnRendererReady(Module::Context& ctx) override
    {
        FaceTracking::Service::Get().SetGraphicsDevice(ctx.gfx);
        m_system = std::make_unique<FaceTrackingSystem>(*ctx.renderer->GetAnimationSystem(),
                                                        ctx.renderer->GetSkeletonRegistry());
        m_hook = std::make_unique<FaceTrackingChainHook>(*m_system);
        ctx.renderer->AddAnimChainHook(m_hook.get());
        ctx.renderer->GetHeadTrackSystem()->SetHeadAuthority(&FaceHeadAuthority);
    }

    void RegisterLua(Module::Context& ctx) override
    {
        FaceTracking::RegisterLuaBindings(*ctx.lua, *ctx.world, ctx.renderer->GetSkeletonRegistry());
    }

    void OnDeviceChange() override { FaceTracking::Service::Get().OnDeviceChange(); }

    // No frame is in flight: close the face sources (and log the harness's probe readers)
    // before anything they could outlive goes.
    void Shutdown() override { FaceTracking::Service::Get().Shutdown(); }

private:
    std::unique_ptr<FaceTrackingSystem>    m_system;
    std::unique_ptr<FaceTrackingChainHook> m_hook;
};

ENGINE_MODULE(FaceTrackingModule)
