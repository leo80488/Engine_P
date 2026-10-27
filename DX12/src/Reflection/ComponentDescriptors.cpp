// ComponentDescriptors.cpp - every descriptor of ComponentReflection.h in the runtime registry
// (Reflection/ReflectRegistry.h), in every exe: Game.exe's catalog dump and tools read the fields too.
// The editor re-registers the components it draws with their inspector labels.

#include "Reflection/ComponentDescriptors.h"

#include "Reflection/ComponentReflection.h"
#include "Reflection/ReflectRegistry.h"

#include <typeindex>

namespace
{
    template <class T>
    void Register()
    {
        const Reflect::TypeDescriptor& d = Reflect::Describe<T>();
        if (!Reflect::FindRuntime(std::type_index(typeid(T))))
            Reflect::RegisterRuntime(std::type_index(typeid(T)), &d, d.name);
    }
}

// One row per REFLECT_BEGIN in ComponentReflection.h, in its order (tests/unit/reflect_descriptor_test.py
// checks that none is missing).
void Reflect::RegisterCoreDescriptors()
{
    Register<VisibilityComponent>();
    Register<RenderLayer>();
    Register<LocalAabb>();
    Register<CameraComponent>();
    Register<CameraControllerComponent>();
    Register<CameraData>();
    Register<VirtualCameraComponent>();
    Register<CameraPoseComponent>();
    Register<VCamPriorityComponent>();
    Register<VCamBlendComponent>();
    Register<FollowCameraComponent>();
    Register<AimCameraComponent>();
    Register<CameraShakeComponent>();
    Register<LiveCameraComponent>();
    Register<BillboardComponent>();
    Register<BillboardFXComponent>();
    Register<WorldEnvironmentComponent>();
    Register<SkyboxComponent>();
    Register<AtmosphereComponent>();
    Register<TODConfigComponent>();
    Register<TODOutputComponent>();
    Register<TODLightComponent>();
    Register<SunLightTag>();
    Register<MoonLightTag>();
    Register<CloudComponent>();
    Register<HeightFogComponent>();
    Register<VolumetricFogComponent>();
    Register<LocalFogVolumeComponent>();
    Register<ReflectionProbeComponent>();
    Register<ProxyVolumeComponent>();
    Register<VolumetricLightComponent>();
    Register<DDGIVolumeComponent>();
    Register<IndirectLightingSettingsComponent>();
    Register<TrailComponent>();
    Register<AnimFXGateComponent>();
    Register<LocalTransform>();
    Register<LightData>();
    Register<MeshHandle>();
    Register<PerObjectShadowComponent>();
    Register<DissolveComponent>();
    Register<HairShadowCasterComponent>();
    Register<FaceOverlayComponent>();
    Register<RigidBodyComponent>();
    Register<CharacterControllerComponent>();
    Register<PlayerComponent>();
    Register<AIIntentComponent>();
    Register<NavAgentComponent>();
    Register<PerceptionComponent>();
    Register<PerceptibleComponent>();
    Register<ColliderComponent>();
    Register<TagComponent>();
    Register<ChainPhysicsComponent>();
    Register<FaceShadingComponent>();
    Register<IKRigComponent>();
    Register<AnimGraphComponent>();
    Register<LocomotionComponent>();
    Register<LocomotionTransitionComponent>();
    Register<CharacterAnimDriverComponent>();
    Register<BrainComponent>();
    Register<AnimLayerComponent>();
    Register<HeadTrackComponent>();
    Register<FacialAnimationComponent>();
    Register<ScriptComponent>();
    Register<AnimationComponent>();
    Register<CameraMotionComponent>();
    Register<MotionDirectorComponent>();
    Register<SequencePlayerComponent>();
    Register<Audio::AudioSourceComponent>();
    Register<Audio::AudioListenerComponent>();
    Register<MorphComponent>();
    Register<SkeletonRef>();
    Register<PrefabSourcePath>();
    Register<SkeletonComponent>();
    Register<CapsuleColliderComponent>();
    Register<SocketComponent>();
    Register<FollowEntityComponent>();
    Register<UI::UICanvas>();
    Register<UI::UIRect>();
    Register<UI::UIClipRect>();
    Register<UI::UIDocument>();
    Register<UI::UIImage>();
    Register<UI::UISpriteAnimComponent>();
    Register<UI::UIText>();
    Register<UI::UIInteractable>();
    Register<UI::WorldSpaceUIComponent>();
    Register<UI::WorldUIBarComponent>();
    Register<UI::WorldUITextComponent>();
    Register<UI::WorldUIImageComponent>();
    Register<UI::DamageNumberComponent>();
    Register<FollowSocketComponent>();
    Register<DecalComponent>();
    Register<TerrainComponent>();
    Register<GrassComponent>();
    Register<WaterInteractionComponent>();
    Register<WaterComponent>();
    Register<BlackboardComponent>();
    Register<AIBehaviorComponent>();
    Register<BeamControlPoint>();
    Register<BeamComponent>();
    Register<ParticleRenderComponent>();
    Register<ParticleEmitterComponent>();
    Register<ParticleFlockComponent>();
    Register<ParticleAmbientComponent>();
    Register<AIComponent>();
    Register<ECS::PostProcessVolumeComponent>();
}
