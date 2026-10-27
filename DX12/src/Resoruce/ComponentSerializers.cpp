#include "ECS/IKRigAuthoring.h"
// ComponentSerializers.cpp: the one list that decides what survives a save.
// This registry is the single source of truth for three features: .iscene / .prefab save + load
// (SceneSerializer), the SaveGame PersistentTag snapshot, and the editor's Undo / Redo
// ComponentBlob. Reflection/ComponentReflection.h (the inspector list) is a separate hand-written
// list, so a component with an inspector but no entry here silently loses its data in all three.
// A component whose line is plain key=value of its reflected fields registers
// RegisterReflectedSerializer<T> (Reflection/ReflectSerializer.h) instead of hand-written code;
// tests/unit/reflect_serializer_test.py holds each tag to the lines its old serializer wrote.
// Deliberate non-entries (do not "fix" them):
//   LiveCameraComponent, CameraStackChannelSingleton - rebuilt every frame by CameraResolveSystem /
//       CameraStackSystem from the VCams.
//   UI::DamageNumberComponent - a transient runtime spawn.
//   BlackboardComponent - registered with has() == false: AI memory is not persisted (see its
//       registration).
// The drift guard between this registry and the descriptors is FindRegistryDrift
// (ComponentRegistryCheck.cpp): it needs no count kept here.

#include "Resource/ComponentSerializers.h"
#include "Reflection/ComponentDescriptors.h"   // registered with the serializers
#include "Reflection/ReflectRegistry.h"
#include "Reflection/ComponentReflection.h"   // the descriptors the reflected serializers write
#include "Reflection/ReflectSerializer.h"
#include "Resource/MaterialOverrideApply.h"   // ParseMaterialOverrideKV, shared with the asset preview
#include "Resource/SceneLoadContext.h"
#include "ECS/Components.h"
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)
#include "ECS/TagComponent.h"
#include "ECS/UnresolvedComponents.h"   // the lines no serializer in this build reads
#include "ECS/CameraStackComponents.h"
#include "ECS/CameraMotionComponent.h"
#include "ECS/MotionDirectorComponent.h"
#include "Sequencer/SequenceComponents.h"
#include "Audio/AudioComponents.h"
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/StaticMeshComponents.h"   // StaticMeshComponent
#include "Resource/MeshLibIdentity.h"   // MeshKeyToHex / MeshKeyFromHex
#include "ECS/AnimationComponents.h"
#include "ECS/AnimGraphComponent.h"
#include "ECS/IKRigComponent.h"
#include "ECS/CharacterStateComponent.h"
#include "ECS/FollowComponents.h"
#include "ECS/FaceShadingComponent.h"
#include "ECS/BillboardComponent.h"
#include "ECS/BillboardFXComponent.h"
#include "ECS/TrailComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/ParticleComponent.h"
#include "ECS/NotifyTypes.h"
#include "ECS/NotifySerialization.h"
#include "System/PathUtil.h"
#include <nlohmann/json.hpp>
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/WorldEnvironment.h"
#include "ECS/EditorFolderComponent.h"
#include "ECS/CloudComponent.h"
#include "ECS/HeightFogComponent.h"
#include "ECS/VolumetricFogComponent.h"
#include "ECS/LocalFogVolumeComponent.h"
#include "ECS/TerrainComponent.h"
#include "ECS/GrassComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/WaterInteractionComponent.h"
#include "ECS/TODComponents.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/ProxyVolumeComponent.h"
#include "ECS/DDGIComponents.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "PostProcess/ProfileSystem.h"
#include "UI/UICanvas.h"
#include "UI/UIDocument.h"      // UI::UIDocument (CvDoc)
#include "UI/WorldSpaceUI.h"
#include "Physics/ChainPhysicsSystem.h"
#include "ECS/PhysicsComponents.h"     // RigidBodyComponent + ColliderComponent
#include "ECS/CharacterControllerComponent.h"
#include "ECS/PlayerComponent.h"
#include "ECS/CharacterIntentComponent.h"   // BrainComponent
#include "ECS/CharacterAnimDriver.h"        // CharacterAnimDriverComponent
#include "ECS/PerObjectShadowComponent.h"
#include "ECS/HairShadowCasterComponent.h"
#include "ECS/AnimFXGateComponent.h"
#include "ECS/FaceOverlayComponent.h"
#include "ECS/DissolveComponent.h"
#include "ECS/AIIntentComponent.h"
#include "ECS/PerceptionComponent.h"
#include "Nav/NavComponents.h"
#include "ECS/HeadTrackComponent.h"    // HeadTrackComponent
#include "ECS/FacialAnimationComponent.h" // FacialAnimationComponent (morph targets)
#include "ECS/FacialAnimationSystem.h"    // Facial::CopyName: one truncation rule
#include "ECS/LocomotionComponents.h"  // LocomotionComponent / AnimLayerComponent
#include "AI/AIComponents.h"
#include "AI/AIBehavior.h"
#include "Scripting/ScriptComponent.h"
#include "Resource/AssetManager.h"
#include "Resource/MaterialSerializer.h"
#include "System/Log.h"
#include "System/Check.h"   // ENGINE_CHECK: the registry drift guard, fires in Release too
#include "Math/MathUtils.h" // RollPitchYawDegreesFromQuaternion (the Sockets Euler seed)
#include "Resource/ComponentKV.h"   // GetF / GetI / GetS / PercentEncode, shared with the modules
#include "Module/ModuleRegistry.h"   // the linked modules' serializers

#include <DirectXMath.h>
#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <memory>

namespace {
// Dummy type that only registers a loader for the combined "ParticleEmitter:" line older scenes
// carry. Never attached to an entity (has() -> false), so never saved; its deserialize migrates the
// line into ParticleRenderComponent + the matching behaviour component.
struct LegacyParticleEmitterTag {};
}
#include <cstring>

using namespace DirectX;

// ==== Helpers ====

// The key/value helpers shared with the modules' serializers (Resource/ComponentKV.h).
using ComponentKV::GetF;
using ComponentKV::GetI;
using ComponentKV::GetS;
using ComponentKV::GetU;
using ComponentKV::PercentDecode;
using ComponentKV::PercentEncode;
using ComponentKV::SanitizeWeight01;

// Strict variant for multi-line payloads embedded as one KV value (the inline material text):
// everything outside a conservative allowlist becomes %XX (newlines, '=', '#', tabs and UTF-8
// bytes included). Decodes with the ordinary PercentDecode.
static std::string PercentEncodeStrict(const std::string& s)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string r; r.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                          c == '/' || c == '-' || c == ':';
        if (safe) r += static_cast<char>(c);
        else { r += '%'; r += kHex[c >> 4]; r += kHex[c & 0xF]; }
    }
    return r;
}

// For author text (a DCC mesh name): '%', every whitespace and control byte
// escaped - a tab or a newline would end the token or the line - and UTF-8
// left readable, unlike PercentEncodeStrict.
static std::string PercentEncodeText(const std::string& s)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string r; r.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '%' || c <= 0x20 || c == 0x7F) { r += '%'; r += kHex[c >> 4]; r += kHex[c & 0xF]; }
        else r += static_cast<char>(c);
    }
    return r;
}

// ==== SceneLoadContext (raw entity-index remap) ====
// SceneSerializer binds the saved-index -> new Entity map for the duration of a load
// (SetLegacyEntityMap); deserializers call ResolveLegacyEntityIdx when they meet a raw-id key
// (camEntity=42, followTarget=42, target=42).

namespace
{
    thread_local const std::unordered_map<int, Entity>* t_legacyMap = nullptr;
}

namespace Resource
{
    void SetLegacyEntityMap(const std::unordered_map<int, Entity>* map)
    {
        t_legacyMap = map;
    }

    Entity ResolveLegacyEntityIdx(uint32_t oldIdx)
    {
        if (!t_legacyMap) return NullEntity;
        auto it = t_legacyMap->find(static_cast<int>(oldIdx));
        return it != t_legacyMap->end() ? it->second : NullEntity;
    }
}

// ==== AttachmentRef serialization helpers ====
// Every entity-ref field writes "<name>Guid=<hex32>". A reader accepts that key (preferred) or an
// older raw-index "<name>" / variant key, which it resolves via SceneLoadContext and then stamps a
// fresh GUID on the target, so the next save is GUID-only.

namespace
{
    void WriteAttachmentRef(std::ostringstream& ss,
                            const char* keyPrefix,   // e.g. "target"
                            World& w,
                            const AttachmentRef& ref)
    {
        // Always write the GUID when there is one; ReverseFind from the cached entity covers a ref bound
        // at runtime without its GUID filled.
        ECS::Guid g = ref.ownerGuid;
        if (!g.IsValid() && ref.cachedOwner != NullEntity)
            g = ECS::GuidRegistry::Get().ReverseFind(ref.cachedOwner);
        if (g.IsValid())
            ss << ' ' << keyPrefix << "Guid=" << g.ToHex();
    }

    // Read an AttachmentRef from @p kv. @p legacyKey is the raw-index key (null when there is none).
    // Resolution order:
    //   1. <keyPrefix>Guid present -> parse and store it, cache left invalid.
    //   2. legacyKey present       -> resolve via SceneLoadContext, stamp the GUID on the target
    //                                 (BindAndStamp).
    //   3. neither                 -> the ref stays empty.
    void ReadAttachmentRef(const KVMap& kv,
                           const char* keyPrefix,
                           const char* legacyKey,
                           World& w,
                           Entity selfEntity,
                           AttachmentRef& out)
    {
        std::string guidKey = std::string(keyPrefix) + "Guid";
        if (auto it = kv.find(guidKey); it != kv.end())
        {
            out.ownerGuid        = ECS::Guid::FromHex(it->second);
            out.cachedOwner      = NullEntity;
            out.cachedGeneration = 0u;
            return;
        }
        if (legacyKey)
        {
            if (auto it = kv.find(legacyKey); it != kv.end())
            {
                uint32_t oldIdx = 0;
                try { oldIdx = static_cast<uint32_t>(std::stoul(it->second)); }
                catch (...) { return; }
                if (oldIdx == 0u) return;
                const Entity remapped = Resource::ResolveLegacyEntityIdx(oldIdx);
                if (remapped != NullEntity)
                {
                    // Stamp the GUID on the target so the next save is GUID-only. A self-reference is fine:
                    // Player.cameraEntity may point at a sibling that has no GUID yet.
                    out.BindAndStamp(w, remapped);
                }
            }
        }
    }
}

// Percent-DECODED already: never wrap it in PercentDecode. A second decode
// turns a literal "%20" (written "%2520") into a space.
// ==== ChainPhysicsComponent: the one authored-field list ====
// The copies (CopyMaterialInto / CopyAuthoredInto) and the compares (MaterialEquals /
// AuthoredEquals) are built on these two X-macros, so they cannot drift apart. The "ChainPhysics"
// serializer in RegisterAllComponentSerializers is not generated from them (its wire keys are frozen
// and carry their own defaults); tests/unit/chain_preview_prefab_test.cpp P7_FieldListLockstep keeps
// the lists and the serializer in lockstep: a blob with every wire key set to a unique non-default
// value must survive serialize(CopyAuthoredInto(parse(blob))) byte for byte, and perturbing any one
// key must flip AuthoredEquals. Add a field to the writer and here.
//
// The physics material: every authored scalar of the component (the group list and the rest-pose
// block are handled separately). X(field).
#define CHAIN_PHYSICS_MATERIAL_FIELDS(X)                                        \
    X(enabled) X(damping) X(gravity) X(structuralStiffness) X(iterations)       \
    X(substeps) X(maxVelocity) X(localStiffness) X(tipHold)                     \
    X(collisionPadding) X(contactSkin) X(contactFriction) X(limitAngle)         \
    X(limitTipScale) X(inertiaScale) X(skirtDamping) X(skirtGravity)            \
    X(ringStiffness) X(shearStiffness) X(skirtMaxVelocity)                      \
    X(skirtLocalStiffness) X(skirtHorizDecay) X(skirtTipHold)                   \
    X(skirtLimitAngle) X(skirtLimitTipScale) X(skirtInertiaScale) X(windDir)    \
    X(windStrength) X(windFreq) X(windTurbulence) X(springStiffness)            \
    X(springDamping) X(springMass) X(springGravity) X(springMaxDisp)            \
    X(springChildStiffness) X(springChildDamping) X(springChildMass)            \
    X(springChildGravity) X(springChildMaxDisp) X(worldInertiaLocation)         \
    X(worldInertiaRotation) X(teleportDistance) X(teleportAngle)
// Every non-array member of ChainGroupDef (all of them are on the g<i>_* wire
// keys). The three char arrays (name, rootBone, excludeBones[excludeCount])
// are compared as C strings beside it. X(member).
#define CHAIN_GROUP_SCALAR_FIELDS(X)                                            \
    X(type) X(enabled) X(excludeCount) X(excludeSubtree) X(ringGroup)           \
    X(ringIndex) X(pairGroupId) X(ovrEnabled) X(ovrDamping) X(ovrGravity)       \
    X(ovrStructuralStiffness) X(ovrRingStiffness) X(ovrShearStiffness)          \
    X(ovrMaxVelocity) X(ovrLocalStiffness) X(ovrHorizDecay) X(ovrTipHold)       \
    X(ovrCollisionPadding) X(ovrContactFriction) X(ovrLimitAngle)               \
    X(ovrLimitTipScale) X(ovrInertiaScale) X(ovrWindStrength) X(ovrWindFreq)    \
    X(ovrSpringStiffness) X(ovrSpringDamping) X(ovrSpringMass)                  \
    X(ovrSpringGravity) X(ovrSpringMaxDisp) X(ovrSpringChildStiffness)          \
    X(ovrSpringChildDamping) X(ovrSpringChildMass) X(ovrSpringChildGravity)     \
    X(ovrSpringChildMaxDisp) X(collide) X(ovrWorldInertiaLocation)              \
    X(ovrWorldInertiaRotation)

namespace
{
    // One equality for every field kind the lists carry. Floats compare with
    // ==: a NaN compares unequal, which only costs one extra sync.
    template <class T> bool ChainFieldEq(const T& a, const T& b) { return a == b; }
    bool ChainFieldEq(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
    { return a.x == b.x && a.y == b.y && a.z == b.z; }
    bool ChainStrEq(const char* a, const char* b, std::size_t n) { return std::strncmp(a, b, n) == 0; }

    bool ChainGroupEq(const ChainGroupDef& a, const ChainGroupDef& b)
    {
#define CHAIN_EQ_X(f) if (!ChainFieldEq(a.f, b.f)) return false;
        CHAIN_GROUP_SCALAR_FIELDS(CHAIN_EQ_X)
#undef CHAIN_EQ_X
        if (!ChainStrEq(a.name, b.name, sizeof(a.name)) ||
            !ChainStrEq(a.rootBone, b.rootBone, sizeof(a.rootBone)))
            return false;
        const int n = std::min(a.excludeCount, ChainGroupDef::MAX_EXCLUDE);
        for (int j = 0; j < n; ++j)
            if (!ChainStrEq(a.excludeBones[j], b.excludeBones[j], sizeof(a.excludeBones[j])))
                return false;
        return true;
    }
}

// CopyMaterialInto: the material only; dst's rest-pose block is left as it was. Every
// ChainGroupDef member is on the g<i>_* wire keys, so the group list is copied whole.
void ChainPhysicsComponent::CopyMaterialInto(ChainPhysicsComponent& dst) const
{
    if (&dst == this) return;
#define CHAIN_COPY_X(f) dst.f = f;
    CHAIN_PHYSICS_MATERIAL_FIELDS(CHAIN_COPY_X)
#undef CHAIN_COPY_X
    dst.groups = groups;
}

// CopyAuthoredInto = the material + the rest-pose block. A plain `dst = *this` would clobber
// BoneRestPose's runtime cache (cachedBoneIdx / resolvedName / resolvedSkel / resolveFailed /
// warnedMissing / appliedThisFrame, see ChainPhysicsSystem.h): dst keeps its cache for an entry
// whose bone name is unchanged at the same index, and a renamed or new entry starts from a default
// cache (it has to re-resolve).
void ChainPhysicsComponent::CopyAuthoredInto(ChainPhysicsComponent& dst) const
{
    if (&dst == this) return;
    CopyMaterialInto(dst);
    dst.restPoseEnabled = restPoseEnabled;
    dst.restPoseWeight  = restPoseWeight;

    // Rest pose: the four authored members of each entry, the runtime cache of
    // the entry dst already had for the same bone name.
    std::vector<BoneRestPose> rp(restPose.size());
    for (std::size_t i = 0; i < restPose.size(); ++i)
    {
        const BoneRestPose& s = restPose[i];
        BoneRestPose&       d = rp[i];
        if (i < dst.restPose.size() &&
            std::strncmp(dst.restPose[i].boneName, s.boneName, sizeof(s.boneName)) == 0)
            d = dst.restPose[i];            // cache and all, authored overwritten below
        std::memcpy(d.boneName, s.boneName, sizeof(d.boneName));
        d.rot     = s.rot;
        d.weight  = s.weight;
        d.enabled = s.enabled;
    }
    dst.restPose = std::move(rp);
}

// MaterialEquals / AuthoredEquals: the same lists compared instead of copied. The preview panel
// syncs its staged copy into the stage's live component, and records a panel-undo step, only when
// AuthoredEquals says they differ. Runtime caches (BoneRestPose's six fields) are ignored, as the
// copy ignores them. Char arrays compare as C strings (bytes past the terminator are not data).
bool ChainPhysicsComponent::MaterialEquals(const ChainPhysicsComponent& o) const
{
    if (&o == this) return true;
#define CHAIN_EQ_X(f) if (!ChainFieldEq(f, o.f)) return false;
    CHAIN_PHYSICS_MATERIAL_FIELDS(CHAIN_EQ_X)
#undef CHAIN_EQ_X
    if (groups.size() != o.groups.size()) return false;
    for (std::size_t i = 0; i < groups.size(); ++i)
        if (!ChainGroupEq(groups[i], o.groups[i])) return false;
    return true;
}

bool ChainPhysicsComponent::AuthoredEquals(const ChainPhysicsComponent& o) const
{
    if (&o == this) return true;
    if (!MaterialEquals(o)) return false;
    if (!(restPoseEnabled == o.restPoseEnabled && restPoseWeight == o.restPoseWeight &&
          restPose.size() == o.restPose.size()))
        return false;
    for (std::size_t i = 0; i < restPose.size(); ++i)
    {
        const BoneRestPose& a = restPose[i];
        const BoneRestPose& b = o.restPose[i];
        if (!(ChainStrEq(a.boneName, b.boneName, sizeof(a.boneName)) && a.rot.x == b.rot.x &&
              a.rot.y == b.rot.y && a.rot.z == b.rot.z && a.rot.w == b.rot.w &&
              a.weight == b.weight && a.enabled == b.enabled))
            return false;
    }
    return true;
}
#undef CHAIN_GROUP_SCALAR_FIELDS
#undef CHAIN_PHYSICS_MATERIAL_FIELDS

// ==== Registration ====

void RegisterAllComponentSerializers(ComponentSerializerRegistry& reg)
{
    // ==== UnresolvedComponents ====
    // The lines ApplyComponentLine kept, one "Unresolved: line=<the line, encoded>" each, so every
    // path that rewrites a component through its serializer (save, undo, Play / Stop, a transfer)
    // carries them. Reading one dispatches the original line again: a build that has its module
    // gets the component back, one that does not keeps it.
    reg.Register(std::type_index(typeid(UnresolvedComponents)), {
        "Unresolved",
        [](World& w, Entity e) { return w.GetComponent<UnresolvedComponents>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            if (const auto* u = w.GetComponent<UnresolvedComponents>(e))
                for (const std::string& line : u->lines)
                    ss << "  Unresolved: line=" << PercentEncodeStrict(line) << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager* assets) {
            const std::string line = GetS(kv, "line", "");
            const size_t colon = line.find(':');
            if (colon == std::string::npos) return;
            const size_t first = line.find_first_not_of(' ');
            const std::string tag = line.substr(first, colon - first);
            KVMap inner;
            std::istringstream tokens(line.substr(colon + 1));
            for (std::string tok; tokens >> tok;)
                if (const size_t eq = tok.find('='); eq != std::string::npos)
                    inner[tok.substr(0, eq)] = tok.substr(eq + 1);
            if (!tag.empty() && tag != "Unresolved")
                ApplyComponentLine(GetComponentRegistry(), w, e, tag, inner, assets);
        }
    });

    // ==== LightData (INLINE) ====
    reg.Register(std::type_index(typeid(LightData)), {
        "LightData",
        [](World& w, Entity e) { return w.GetComponent<LightData>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ld = w.GetComponent<LightData>(e);
            if (!ld) return;
            // No `dir=`: orientation lives on the entity transform, which the N line already saved (see
            // LightUtil.h).
            char buf[512]; // All float32 fields at their finite maximum still fit.
            // sourceRadius is written only when > 0, so a punctual light's line carries no sourceRadius key.
            char extra[64] = "";
            if (ld->sourceRadius > 0.f) snprintf(extra, sizeof(extra), " sourceRadius=%.4f", ld->sourceRadius);
            snprintf(buf, sizeof(buf),
                "  LightData: type=%u radius=%.4f intensity=%.4f"
                " color=%.4f_%.4f_%.4f spotAngle=%.4f"
                " castsShadow=%u enabled=%u%s\n",
                static_cast<uint32_t>(ld->type), ld->radius, ld->intensity,
                ld->color.x, ld->color.y, ld->color.z,
                ld->spotAngle,
                ld->castsShadow ? 1u : 0u, ld->enabled ? 1u : 0u, extra);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            LightData ld;
            ld.type        = static_cast<LightType>(GetI(kv, "type", 0));
            ld.radius      = GetF(kv, "radius", 10.f);
            ld.intensity   = GetF(kv, "intensity", 1.f);
            ld.spotAngle   = GetF(kv, "spotAngle", 0.5236f);
            ld.castsShadow = GetI(kv, "castsShadow", 0) != 0;
            ld.enabled = GetI(kv, "enabled", 1) != 0;
            const float sourceRadius = GetF(kv, "sourceRadius", 0.f);
            ld.sourceRadius = std::isfinite(sourceRadius) && sourceRadius > 0.f ? sourceRadius : 0.f;
            // Parse color "r_g_b"
            auto cit = kv.find("color");
            if (cit != kv.end()) sscanf_s(cit->second.c_str(), "%f_%f_%f", &ld.color.x, &ld.color.y, &ld.color.z);
            w.AddComponent<LightData>(e, ld);

            // Migration: a `dir=` key (older scenes) is folded into the entity's rotation so old worlds keep
            // pointing where they were authored; the next save drops the key. Writing LocalTransform here is
            // safe: SceneSerializer adds it from the entity's N line before any component block runs, and the
            // old direction was in the entity's local space, which is what this rotation means.
            auto dit = kv.find("dir");
            if (dit == kv.end()) return;
            DirectX::XMFLOAT3 legacyDir{ 0.f, -1.f, 0.f };
            if (sscanf_s(dit->second.c_str(), "%f_%f_%f",
                         &legacyDir.x, &legacyDir.y, &legacyDir.z) != 3) return;
            LocalTransform* lt = w.GetComponent<LocalTransform>(e);
            if (!lt) return;
            // Compose, so a rotation already on the entity still applies: the old dir was local, so aim in
            // local space and let the parent chain do its job.
            const DirectX::XMFLOAT4 aim = LightUtil::RotationAiming(legacyDir);
            DirectX::XMStoreFloat4(&lt->rotation,
                DirectX::XMQuaternionMultiply(
                    DirectX::XMLoadFloat4(&aim),
                    DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&lt->rotation))));
        }
    });

    // ==== VolumetricLightComponent (INLINE) ====
    // Opt-in marker: a Light entity also contributes to the volumetric fog froxel grid.
    RegisterReflectedSerializer<VolumetricLightComponent>(reg, "VolumetricLight", Reflect::NumberStyle::Fixed4);

    // ==== ReflectionProbeComponent (INLINE) ====
    // Persists the authored influence / fade bounds so a saved scene restores probe placement exactly.
    // cubemapSlice is runtime-only (the Renderer assigns a slot after load) and the BAKED flag is
    // dropped, but bakedCubemapPath (the exported .itex, key bakedCubemap=) and bakeVersion are
    // persisted: Renderer::ExportBakedProbeCubemaps stamps the path right before SaveScene, and after the
    // next load BuildScene_SelectProbes restores the cubemap from disk when bakeVersion is current, so a
    // non-realtime probe skips a 6-face re-bake on every load.
    reg.Register(std::type_index(typeid(ReflectionProbeComponent)), {
        "ReflectionProbe",
        [](World& w, Entity e) { return w.GetComponent<ReflectionProbeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* rp = w.GetComponent<ReflectionProbeComponent>(e);
            if (!rp) return;
            char buf[640];
            snprintf(buf, sizeof(buf),
                "  ReflectionProbe: ver=3"
                " ihx=%.4f ihy=%.4f ihz=%.4f"
                " fpx=%.4f fpy=%.4f fpz=%.4f fnx=%.4f fny=%.4f fnz=%.4f"
                " ishape=%u ior=%.4f iir=%.4f"
                " cox=%.4f coy=%.4f coz=%.4f"
                " prio=%u faceRes=%u intensity=%.4f"
                " realtime=%u tickIntervalFrames=%u bakeVersion=%u"
                " showViz=%u",
                rp->influenceHalfExtents.x, rp->influenceHalfExtents.y, rp->influenceHalfExtents.z,
                rp->fadePos.x, rp->fadePos.y, rp->fadePos.z,
                rp->fadeNeg.x, rp->fadeNeg.y, rp->fadeNeg.z,
                static_cast<uint32_t>(rp->influenceShape),
                rp->influenceOuterRadius, rp->influenceInnerRadius,
                rp->captureOffsetLS.x, rp->captureOffsetLS.y, rp->captureOffsetLS.z,
                static_cast<uint32_t>(rp->priority), static_cast<uint32_t>(rp->faceResolution),
                rp->intensity,
                rp->realtime ? 1u : 0u, rp->tickIntervalFrames, rp->bakeDataVersion,
                rp->showVizSphere ? 1u : 0u);
            ss << buf;
            // proxyVolume target is GUID-stamped by SceneSerializer's pre-stamp pass.
            WriteAttachmentRef(ss, "proxy", w, rp->proxyVolume);
            // Path is percent-encoded so spaces / '%' survive the KV line.
            if (!rp->bakedCubemapPath.empty())
                ss << " bakedCubemap=" << PercentEncode(rp->bakedCubemapPath);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ReflectionProbeComponent rp;
            auto parseVec3 = [&](const char* kx, const char* ky, const char* kz, XMFLOAT3& out) {
                out.x = GetF(kv, kx, out.x);
                out.y = GetF(kv, ky, out.y);
                out.z = GetF(kv, kz, out.z);
            };
            if (kv.find("ver") != kv.end())
            {
                parseVec3("ihx", "ihy", "ihz", rp.influenceHalfExtents);
                parseVec3("fpx", "fpy", "fpz", rp.fadePos);
                parseVec3("fnx", "fny", "fnz", rp.fadeNeg);
                rp.influenceShape = static_cast<EInfluenceShape>(GetI(kv, "ishape", 0));
                rp.influenceOuterRadius = GetF(kv, "ior", rp.influenceOuterRadius);
                rp.influenceInnerRadius = GetF(kv, "iir", rp.influenceInnerRadius);
                parseVec3("cox", "coy", "coz", rp.captureOffsetLS);
                rp.priority       = static_cast<uint8_t>(GetI(kv, "prio", 0));
                rp.faceResolution = static_cast<uint16_t>(GetI(kv, "faceRes", 128));
            }
            else
            {
                // Migration (a line without ver=): comma-packed innerExtents / outerExtents ->
                // influenceHalfExtents = outer, fade = max(outer - inner, 0), Box shape, no proxy (the upload
                // synthesizes an OBB from the influence box).
                XMFLOAT3 inner{ 4, 2, 4 }, outer{ 6, 3, 6 };
                auto legacyVec3 = [&](const char* key, XMFLOAT3& o) {
                    auto it = kv.find(key); if (it == kv.end()) return;
                    float x = o.x, y = o.y, z = o.z;
                    if (sscanf_s(it->second.c_str(), "%f,%f,%f", &x, &y, &z) >= 1) o = { x, y, z };
                };
                legacyVec3("innerExtents", inner);
                legacyVec3("outerExtents", outer);
                outer = { std::max(outer.x, inner.x), std::max(outer.y, inner.y), std::max(outer.z, inner.z) };
                rp.influenceHalfExtents = outer;
                rp.fadePos = rp.fadeNeg = { std::max(0.0f, outer.x - inner.x),
                                            std::max(0.0f, outer.y - inner.y),
                                            std::max(0.0f, outer.z - inner.z) };
                rp.influenceShape = EInfluenceShape::Box;
            }
            rp.intensity          = GetF(kv, "intensity", 1.0f);
            rp.realtime           = GetI(kv, "realtime", 0) != 0;
            rp.tickIntervalFrames = static_cast<uint32_t>(GetI(kv, "tickIntervalFrames", 60));
            rp.bakeDataVersion    = static_cast<uint32_t>(GetI(kv, "bakeVersion", 0));
            // Absent in older scenes, which recorded a hidden viz ball only in the entity's
            // VisibilityComponent; that row is parsed after this one, so flag it and let the editor's first
            // sync seed showVizSphere from the entity. Otherwise such a scene would turn a deliberately hidden
            // probe ball back on.
            const bool hasShowViz = kv.find("showViz") != kv.end();
            rp.showVizSphere         = !hasShowViz || GetI(kv, "showViz", 1) != 0;
            rp.vizSeedFromVisibility = !hasShowViz;
            ReadAttachmentRef(kv, "proxy", nullptr, w, e, rp.proxyVolume);
            auto bc = kv.find("bakedCubemap");
            if (bc != kv.end())
                rp.bakedCubemapPath = PercentDecode(bc->second);
            w.AddComponent<ReflectionProbeComponent>(e, rp);
        }
    });

    // ==== ProxyVolumeComponent (INLINE) ====
    // Parallax-proxy geometry, shareable across probes via AttachmentRef. The per-frame rigid matrices
    // are derived in the upload system, so `revision` (the dirty gate) is not serialized.
    reg.Register(std::type_index(typeid(ProxyVolumeComponent)), {
        "ProxyVolume",
        [](World& w, Entity e) { return w.GetComponent<ProxyVolumeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<ProxyVolumeComponent>(e);
            if (!p) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  ProxyVolume: shape=%u ox=%.4f oy=%.4f oz=%.4f"
                " hx=%.4f hy=%.4f hz=%.4f radius=%.4f\n",
                static_cast<uint32_t>(p->shape),
                p->centerOffsetLS.x, p->centerOffsetLS.y, p->centerOffsetLS.z,
                p->halfExtents.x, p->halfExtents.y, p->halfExtents.z,
                p->sphereRadius);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ProxyVolumeComponent p;
            p.shape = static_cast<EProxyShape>(GetI(kv, "shape", 2)); // default OrientedBox
            p.centerOffsetLS = { GetF(kv, "ox", 0.0f), GetF(kv, "oy", 0.0f), GetF(kv, "oz", 0.0f) };
            p.halfExtents    = { GetF(kv, "hx", p.halfExtents.x),
                                 GetF(kv, "hy", p.halfExtents.y),
                                 GetF(kv, "hz", p.halfExtents.z) };
            p.sphereRadius   = GetF(kv, "radius", p.sphereRadius);
            w.AddComponent<ProxyVolumeComponent>(e, p);
        }
    });

    // ==== DDGIVolumeComponent (INLINE) ====
    // Authored DDGI volume params. DDGIVolumeRuntimeComponent (GPU resource handles) is not
    // serialized: Renderer's BuildScene_UpdateDDGI adds it on the next tick to a DDGIVolumeComponent
    // without one. Keys mirror the struct field names where short, with 2-letter keys for vec3 parts
    // (ox/oy/oz, ex/ey/ez, tx/ty/tz).
    reg.Register(std::type_index(typeid(DDGIVolumeComponent)), {
        "DDGIVolume",
        [](World& w, Entity e) { return w.GetComponent<DDGIVolumeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* v = w.GetComponent<DDGIVolumeComponent>(e);
            if (!v) return;
            char buf[640];
            snprintf(buf, sizeof(buf),
                "  DDGIVolume: ox=%.4f oy=%.4f oz=%.4f"
                " ex=%.4f ey=%.4f ez=%.4f"
                " px=%u py=%u pz=%u rays=%u updiv=%u ucomp=%.4f"
                " hyst=%.4f jit=%.4f nb=%.4f vb=%.4f bfr=%.4f pbw=%.4f bss=%.4f crb=%.4f"
                // debugDraw is not serialized: an editor-only debug toggle, not authoring data.
                " reloc=%u classify=%u adaptRays=%u prio=%d cascade=%u"
                " tx=%.4f ty=%.4f tz=%.4f tscale=%.4f\n",
                v->origin.x, v->origin.y, v->origin.z,
                v->extent.x, v->extent.y, v->extent.z,
                v->probeCountsX, v->probeCountsY, v->probeCountsZ, v->raysPerProbe,
                v->probeUpdateDivisor, v->updateCompensation,
                v->hysteresis, v->rotationJitterScale,
                v->normalBias, v->viewBias, v->boundaryFadeRatio,
                v->perceptualBlendWeight, v->biasSpacingScale,
                v->changeResponseBoost,
                v->enableRelocation     ? 1u : 0u,
                v->enableClassification ? 1u : 0u,
                v->enableAdaptiveRayBudget ? 1u : 0u,
                v->priority, v->cascadeLevel,
                v->diffuseTint.x, v->diffuseTint.y, v->diffuseTint.z, v->diffuseScale);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            DDGIVolumeComponent v;
            v.origin = { GetF(kv, "ox", v.origin.x), GetF(kv, "oy", v.origin.y), GetF(kv, "oz", v.origin.z) };
            v.extent = { GetF(kv, "ex", v.extent.x), GetF(kv, "ey", v.extent.y), GetF(kv, "ez", v.extent.z) };
            v.probeCountsX = static_cast<uint32_t>(GetI(kv, "px",   static_cast<int>(v.probeCountsX)));
            v.probeCountsY = static_cast<uint32_t>(GetI(kv, "py",   static_cast<int>(v.probeCountsY)));
            v.probeCountsZ = static_cast<uint32_t>(GetI(kv, "pz",   static_cast<int>(v.probeCountsZ)));
            v.raysPerProbe = static_cast<uint32_t>(GetI(kv, "rays", static_cast<int>(v.raysPerProbe)));
            // Missing "updiv" -> 1: every probe every frame.
            v.probeUpdateDivisor = static_cast<uint32_t>(
                GetI(kv, "updiv", static_cast<int>(v.probeUpdateDivisor)));
            // Missing "ucomp" -> 1: full response-preserving divisor compensation.
            v.updateCompensation = GetF(kv, "ucomp", v.updateCompensation);
            v.hysteresis        = GetF(kv, "hyst", v.hysteresis);
            v.rotationJitterScale = GetF(kv, "jit", v.rotationJitterScale);
            v.normalBias        = GetF(kv, "nb",   v.normalBias);
            v.viewBias          = GetF(kv, "vb",   v.viewBias);
            v.boundaryFadeRatio = GetF(kv, "bfr",  v.boundaryFadeRatio);
            // Missing "pbw" -> 1.0: the reference perceptual blend.
            v.perceptualBlendWeight = GetF(kv, "pbw", v.perceptualBlendWeight);
            // Missing "bss" -> 0: absolute biases only.
            v.biasSpacingScale      = GetF(kv, "bss", v.biasSpacingScale);
            // Missing "crb" -> 0: plain EMA.
            v.changeResponseBoost   = GetF(kv, "crb", v.changeResponseBoost);
            v.enableRelocation     = GetI(kv, "reloc",    v.enableRelocation     ? 1 : 0) != 0;
            v.enableClassification = GetI(kv, "classify", v.enableClassification ? 1 : 0) != 0;
            v.enableAdaptiveRayBudget = GetI(kv, "adaptRays", v.enableAdaptiveRayBudget ? 1 : 0) != 0;
            v.priority      = GetI(kv, "prio",    v.priority);
            v.cascadeLevel  = static_cast<uint32_t>(GetI(kv, "cascade", static_cast<int>(v.cascadeLevel)));
            // debugDraw is editor-only and not persisted; a "dbg=" token in an old scene is ignored
            // (-> false).
            v.diffuseTint   = { GetF(kv, "tx", v.diffuseTint.x),
                                GetF(kv, "ty", v.diffuseTint.y),
                                GetF(kv, "tz", v.diffuseTint.z) };
            v.diffuseScale  = GetF(kv, "tscale", v.diffuseScale);
            w.AddComponent<DDGIVolumeComponent>(e, v);
        }
    });

    // ==== DDGISceneTagComponent (INLINE, TLAS-membership marker) ====
    // Presence is the state; `reserved` is written so a per-entity field can be added without a
    // format change.
    reg.Register(std::type_index(typeid(DDGISceneTagComponent)), {
        "DDGISceneTag",
        [](World& w, Entity e) { return w.GetComponent<DDGISceneTagComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* t = w.GetComponent<DDGISceneTagComponent>(e);
            if (!t) return;
            char buf[64];
            snprintf(buf, sizeof(buf), "  DDGISceneTag: reserved=%u\n",
                     static_cast<unsigned>(t->reserved));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            DDGISceneTagComponent t;
            t.reserved = static_cast<uint8_t>(GetI(kv, "reserved", 0));
            w.AddComponent<DDGISceneTagComponent>(e, t);
        }
    });

    // ==== IndirectLightingSettingsComponent (INLINE, scene singleton) ====
    // Renderer::BuildScene_UpdateDDGI copies the first instance in the pool into m_ddgiSettings each
    // frame, so one entity carrying it drives the engine-wide indirect-lighting knobs (its ddgi / ssgi
    // capped by the player's features.ddgi / ssgi).
    reg.Register(std::type_index(typeid(IndirectLightingSettingsComponent)), {
        "IndirectLighting",
        [](World& w, Entity e) { return w.GetComponent<IndirectLightingSettingsComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* s = w.GetComponent<IndirectLightingSettingsComponent>(e);
            if (!s) return;
            char buf[800];   // keep ahead of the growing indirect-lighting settings line
            snprintf(buf, sizeof(buf),
                "  IndirectLighting: ddgi=%u ddgiScale=%.4f"
                " ddgiAONear=%.4f"
                " probePri=%u ddgiRoughSpec=%u probeBakeAmb=%.4f"
                " nprMaxBright=%.4f"
                " ssgi=%u ssgiInt=%.4f ssgiDist=%.4f ssgiRays=%u"
                " ssgiRes=%u ssgiResN=%u ssgiResM=%u ssgiResAge=%u ssgiResVis=%u"
                " ssgiThick=%.4f ssgiDbg=%u\n",
                s->ddgiEnabled ? 1u : 0u,
                s->ddgiDiffuseScale,
                s->ddgiAONearFieldStrength,
                s->reflectionProbePriorityOverDDGI ? 1u : 0u,
                s->useDDGIForRoughSpecularFallback ? 1u : 0u,
                s->reflectionProbeBakeAmbient,
                s->nprMaxBrightness,
                s->ssgiEnabled ? 1u : 0u,
                s->ssgiIntensity,
                s->ssgiMaxDistance,
                s->ssgiRayCount,
                s->ssgiReservoirEnabled ? 1u : 0u,
                s->ssgiReservoirSpatialCandidates,
                s->ssgiReservoirMaxM,
                s->ssgiReservoirMaxAge,
                s->ssgiReservoirValidationSteps,
                s->ssgiThickness,
                s->ssgiDebugMode);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            IndirectLightingSettingsComponent s;
            s.ddgiEnabled              = GetI(kv, "ddgi",        s.ddgiEnabled ? 1 : 0) != 0;
            s.ddgiDiffuseScale         = GetF(kv, "ddgiScale",   s.ddgiDiffuseScale);
            // An old file's `skyScale` key (no such field) is not read: an unused KVMap entry.
            s.ddgiAONearFieldStrength  = GetF(kv, "ddgiAONear",  s.ddgiAONearFieldStrength);
            // Old `ssr` / `ssrCut` / `ssrEdge` keys are not read either: unused KVMap entries.
            s.reflectionProbePriorityOverDDGI =
                GetI(kv, "probePri",      s.reflectionProbePriorityOverDDGI ? 1 : 0) != 0;
            s.useDDGIForRoughSpecularFallback =
                GetI(kv, "ddgiRoughSpec", s.useDDGIForRoughSpecularFallback ? 1 : 0) != 0;
            s.reflectionProbeBakeAmbient = GetF(kv, "probeBakeAmb", s.reflectionProbeBakeAmbient);
            s.nprMaxBrightness           = GetF(kv, "nprMaxBright", s.nprMaxBrightness);
            // Absent keys keep the struct defaults, so a scene without them loads with SSGI off.
            s.ssgiEnabled     = GetI(kv, "ssgi",      s.ssgiEnabled ? 1 : 0) != 0;
            s.ssgiIntensity   = GetF(kv, "ssgiInt",   s.ssgiIntensity);
            s.ssgiMaxDistance = GetF(kv, "ssgiDist",  s.ssgiMaxDistance);
            s.ssgiRayCount    = (uint32_t)GetI(kv, "ssgiRays", (int)s.ssgiRayCount);
            s.ssgiReservoirEnabled =
                GetI(kv, "ssgiRes", s.ssgiReservoirEnabled ? 1 : 0) != 0;
            s.ssgiReservoirSpatialCandidates = (uint32_t)GetI(
                kv, "ssgiResN", (int)s.ssgiReservoirSpatialCandidates);
            s.ssgiReservoirMaxM = (uint32_t)GetI(
                kv, "ssgiResM", (int)s.ssgiReservoirMaxM);
            s.ssgiReservoirMaxAge = (uint32_t)GetI(
                kv, "ssgiResAge", (int)s.ssgiReservoirMaxAge);
            s.ssgiReservoirValidationSteps = (uint32_t)GetI(
                kv, "ssgiResVis", (int)s.ssgiReservoirValidationSteps);
            s.ssgiThickness   = GetF(kv, "ssgiThick", s.ssgiThickness);
            s.ssgiDebugMode   = (uint32_t)GetI(kv, "ssgiDbg", (int)s.ssgiDebugMode);
            w.AddComponent<IndirectLightingSettingsComponent>(e, s);
        }
    });

    // ==== ECS::PostProcessVolumeComponent (INLINE) ====
    // Post-process spatial volume. Bounds come from the entity's Transform, so
    // only the volume's own knobs + the referenced .ppprofile asset path are
    // serialized. On load the path is resolved to a ProfileHandle via
    // ProfileSystem::Acquire (path-deduped, so many volumes share one profile).
    reg.Register(std::type_index(typeid(ECS::PostProcessVolumeComponent)), {
        "PPVolume",
        [](World& w, Entity e) { return w.GetComponent<ECS::PostProcessVolumeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* vc = w.GetComponent<ECS::PostProcessVolumeComponent>(e);
            if (!vc) return;

            char buf[512];
            snprintf(buf, sizeof(buf),
                "  PPVolume: isGlobal=%u shape=%u priority=%.4f"
                " blendWeight=%.4f blendDistance=%.4f layerMask=%u profile=%s\n",
                vc->isGlobal ? 1u : 0u,
                static_cast<unsigned>(vc->shape),
                vc->priority, vc->blendWeight, vc->blendDistance,
                vc->layerMask,
                PercentEncode(Sys::RelativizeAssetPath(vc->profilePath)).c_str());
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ECS::PostProcessVolumeComponent vc;
            vc.isGlobal      = GetI(kv, "isGlobal", 0) != 0;
            vc.shape         = static_cast<ECS::PPVolumeShape>(GetI(kv, "shape", 0));
            vc.priority      = GetF(kv, "priority", 0.0f);
            vc.blendWeight   = GetF(kv, "blendWeight", 1.0f);
            vc.blendDistance = GetF(kv, "blendDistance", 1.0f);
            vc.layerMask     = GetU(kv, "layerMask", 0xFFFFFFFFu);
            // An older scene may hold an absolute dialog path here, which no pak key matches: relativize it.
            vc.profilePath   = Sys::RelativizeAssetPath(GetS(kv, "profile", ""));
            if (!vc.profilePath.empty())
                vc.profile = PostProcess::ProfileSystem::Get().Acquire(vc.profilePath);

            w.AddComponent<ECS::PostProcessVolumeComponent>(e, vc);
        }
    });

    // ==== BillboardComponent (INLINE) ====
    reg.Register(std::type_index(typeid(BillboardComponent)), {
        "Billboard",
        [](World& w, Entity e) { return w.GetComponent<BillboardComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* bb = w.GetComponent<BillboardComponent>(e);
            if (!bb) return;
            char buf[128];
            snprintf(buf, sizeof(buf), "  Billboard: mode=%u worldSize=%.4f\n",
                     static_cast<uint32_t>(bb->mode), bb->worldSize);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            BillboardComponent bb;
            bb.mode      = static_cast<BillboardMode>(GetI(kv, "mode", 2));
            bb.worldSize = GetF(kv, "worldSize", 1.f);
            w.AddComponent<BillboardComponent>(e, bb);
        }
    });

    // ==== BillboardFXComponent (INLINE, animated sprite-sheet billboard) ====
    reg.Register(std::type_index(typeid(BillboardFXComponent)), {
        "BillboardFX",
        [](World& w, Entity e) { return w.GetComponent<BillboardFXComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* b = w.GetComponent<BillboardFXComponent>(e);
            if (!b) return;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "  BillboardFX: cols=%d rows=%d frames=%d fps=%.4f size=%.4f"
                " tint=%.4f_%.4f_%.4f_%.4f emissive=%.4f opacity=%.4f"
                " face=%u blend=%u play=%u depthTest=%u playing=%u",
                b->columns, b->rows, b->frameCount, b->fps, b->size,
                b->tint.x, b->tint.y, b->tint.z, b->tint.w, b->emissive, b->opacity,
                static_cast<uint32_t>(b->face), static_cast<uint32_t>(b->blend),
                static_cast<uint32_t>(b->playback),
                b->depthTest ? 1u : 0u, b->playing ? 1u : 0u);
            ss << buf;
            // Texture path percent-encoded so spaces / '%' survive the KV line.
            if (!b->texturePath.empty())
                ss << " texturePath=" << PercentEncode(b->texturePath);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            BillboardFXComponent b;
            b.columns    = GetI(kv, "cols",   b.columns);
            b.rows       = GetI(kv, "rows",   b.rows);
            b.frameCount = GetI(kv, "frames", b.frameCount);
            b.fps        = GetF(kv, "fps",    b.fps);
            b.size       = GetF(kv, "size",   b.size);
            if (auto it = kv.find("tint"); it != kv.end())
                sscanf_s(it->second.c_str(), "%f_%f_%f_%f",
                         &b.tint.x, &b.tint.y, &b.tint.z, &b.tint.w);
            b.emissive   = GetF(kv, "emissive", b.emissive);
            b.opacity    = GetF(kv, "opacity",  b.opacity);
            b.face       = static_cast<BillboardFXFace>(GetI(kv, "face",  0));
            b.blend      = static_cast<BillboardFXBlend>(GetI(kv, "blend", 1));
            b.playback   = static_cast<BillboardFXPlayback>(GetI(kv, "play", 0));
            b.depthTest  = GetI(kv, "depthTest", 1) != 0;
            b.playing    = GetI(kv, "playing",   1) != 0;
            // texturePath stays as a path; BillboardFXPass resolves the bindless
            // slot + GPU handle on its next frame after the async load is ready.
            b.texturePath        = GetS(kv, "texturePath");
            b.textureBindlessIdx = -1;
            b.textureGpuHandle   = 0;
            w.AddComponent<BillboardFXComponent>(e, b);
        }
    });

    // ==== TrailComponent (INLINE) ====
    // Ribbon-trail visual + sampling params. trailSlot, lastSamplePos and the other CPU-only fields
    // are runtime state TrailSystem sets up on first sight, so they are not persisted. Every key beyond
    // the original set is optional with the struct default, so older scenes load pixel-identical and
    // an older build reads a newer scene as a plain trail (unknown keys are ignored). Append new
    // fields via the stream, not the fixed snprintf buffer: a silently truncated writer produces
    // loadable but corrupt scenes.
    reg.Register(std::type_index(typeid(TrailComponent)), {
        "Trail",
        [](World& w, Entity e) { return w.GetComponent<TrailComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* tc = w.GetComponent<TrailComponent>(e);
            if (!tc) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  Trail: enabled=%u width=%.4f maxAge=%.4f minSampleDistance=%.4f"
                " startColor=%.4f_%.4f_%.4f_%.4f endColor=%.4f_%.4f_%.4f_%.4f",
                tc->enabled ? 1u : 0u, tc->width, tc->maxAge, tc->minSampleDistance,
                tc->startColor.x, tc->startColor.y, tc->startColor.z, tc->startColor.w,
                tc->endColor.x,   tc->endColor.y,   tc->endColor.z,   tc->endColor.w);
            ss << buf;
            char pt[256];
            snprintf(pt, sizeof(pt),
                " pointCount=%u p0=%.4f_%.4f_%.4f p1=%.4f_%.4f_%.4f"
                " p2=%.4f_%.4f_%.4f p3=%.4f_%.4f_%.4f",
                tc->pointCount,
                tc->localPoints[0].x, tc->localPoints[0].y, tc->localPoints[0].z,
                tc->localPoints[1].x, tc->localPoints[1].y, tc->localPoints[1].z,
                tc->localPoints[2].x, tc->localPoints[2].y, tc->localPoints[2].z,
                tc->localPoints[3].x, tc->localPoints[3].y, tc->localPoints[3].z);
            ss << pt;
            if (!tc->texturePath.empty())
                ss << " texturePath=" << PercentEncode(tc->texturePath);
            if (!tc->customShaderPath.empty())
            {
                ss << " customShader=" << PercentEncode(tc->customShaderPath);
                // Shader-param overrides, diffed against the default: an entry equal to the shader's //@default
                // (per runtime meta) is skipped. A component not yet synced (saved before its first render) has
                // empty meta and writes everything (a safe superset).
                for (const auto& [name, v] : tc->customParams)
                {
                    auto mit = tc->customParamMeta.find(name);
                    if (mit != tc->customParamMeta.end())
                    {
                        const auto& d = mit->second.defaultValue;
                        if (v.x == d.x && v.y == d.y && v.z == d.z && v.w == d.w)
                            continue;
                    }
                    char pb[192];
                    snprintf(pb, sizeof(pb), " cparam_%s=%.4f_%.4f_%.4f_%.4f",
                             PercentEncode(name).c_str(), v.x, v.y, v.z, v.w);
                    ss << pb;
                }
                // Texture-slot params ("Tex_*"): slot name -> asset path.
                for (const auto& [name, path] : tc->customTextures)
                    if (!path.empty())
                        ss << " ctex_" << PercentEncode(name)
                           << "=" << PercentEncode(path);
            }
            char ap[192];
            snprintf(ap, sizeof(ap),
                " uvMode=%u uvTileLength=%.4f blendMode=%u hdrIntensity=%.4f"
                " taaExclude=%u subdivisions=%u subSampleAngleDeg=%.4f\n",
                tc->uvMode, tc->uvTileLength, tc->blendMode, tc->hdrIntensity,
                tc->excludeFromTAA ? 1u : 0u, tc->subdivisions, tc->subSampleAngleDeg);
            ss << ap;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            TrailComponent tc;
            tc.enabled           = GetI(kv, "enabled", 1) != 0;
            tc.width             = GetF(kv, "width", 0.15f);
            tc.maxAge            = GetF(kv, "maxAge", 1.5f);
            tc.minSampleDistance = GetF(kv, "minSampleDistance", 0.02f);
            auto sit = kv.find("startColor");
            if (sit != kv.end())
                sscanf_s(sit->second.c_str(), "%f_%f_%f_%f",
                         &tc.startColor.x, &tc.startColor.y, &tc.startColor.z, &tc.startColor.w);
            auto eit = kv.find("endColor");
            if (eit != kv.end())
                sscanf_s(eit->second.c_str(), "%f_%f_%f_%f",
                         &tc.endColor.x, &tc.endColor.y, &tc.endColor.z, &tc.endColor.w);
            tc.pointCount = static_cast<uint32_t>(GetI(kv, "pointCount", 1));
            if (tc.pointCount < 1) tc.pointCount = 1;
            if (tc.pointCount > 4) tc.pointCount = 4;
            for (int p = 0; p < 4; ++p)
            {
                char key[8];
                snprintf(key, sizeof(key), "p%d", p);
                auto pit = kv.find(key);
                if (pit != kv.end())
                    sscanf_s(pit->second.c_str(), "%f_%f_%f",
                             &tc.localPoints[p].x, &tc.localPoints[p].y, &tc.localPoints[p].z);
            }
            tc.texturePath       = GetS(kv, "texturePath");
            tc.customShaderPath  = GetS(kv, "customShader");
            for (const auto& [k, val] : kv)
            {
                if (k.rfind("cparam_", 0) == 0)
                {
                    DirectX::XMFLOAT4 v{ 0, 0, 0, 0 };
                    if (sscanf_s(val.c_str(), "%f_%f_%f_%f", &v.x, &v.y, &v.z, &v.w) == 4)
                        tc.customParams[PercentDecode(k.substr(7))] = v;
                }
                else if (k.rfind("ctex_", 0) == 0)
                {
                    tc.customTextures[PercentDecode(k.substr(5))] = PercentDecode(val);
                }
            }
            tc.uvMode            = static_cast<uint32_t>(GetI(kv, "uvMode", 1));
            tc.uvTileLength      = GetF(kv, "uvTileLength", 0.0f);
            tc.blendMode         = static_cast<uint32_t>(GetI(kv, "blendMode", 0));
            tc.hdrIntensity      = GetF(kv, "hdrIntensity", 1.0f);
            tc.excludeFromTAA    = GetI(kv, "taaExclude", 0) != 0;
            tc.subdivisions      = static_cast<uint32_t>(GetI(kv, "subdivisions", 4));
            if (tc.subdivisions < 1) tc.subdivisions = 1;
            if (tc.subdivisions > 4) tc.subdivisions = 4;
            tc.subSampleAngleDeg = GetF(kv, "subSampleAngleDeg", 15.0f);
            w.AddComponent<TrailComponent>(e, tc);
        }
    });

    // ==== BeamComponent (INLINE) ====
    // Procedural tube beam. controlPoints is a variable-length list, written like MotionDirector's
    // tracks: an explicit `count=` plus per-index keys, so the component stays on one KV line without
    // nested container syntax. `beamSlot` is a BeamSystem slot re-acquired on first sight and never
    // saved: a stale slot id would alias another entity's GPU beam.
    reg.Register(std::type_index(typeid(BeamComponent)), {
        "Beam",
        [](World& w, Entity e) { return w.GetComponent<BeamComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* b = w.GetComponent<BeamComponent>(e);
            if (!b) return;
            char hdr[160];
            snprintf(hdr, sizeof(hdr),
                "  Beam: radiusScale=%.4f wobbleAmp=%.4f wobbleSpeed=%.4f count=%zu",
                b->globalRadiusScale, b->wobbleAmplitude, b->wobbleSpeed,
                b->controlPoints.size());
            ss << hdr;
            for (size_t i = 0; i < b->controlPoints.size(); ++i)
            {
                const BeamControlPoint& cp = b->controlPoints[i];
                char pb[224];
                snprintf(pb, sizeof(pb),
                    " p%zu=%.4f_%.4f_%.4f r%zu=%.4f c%zu=%.4f_%.4f_%.4f_%.4f",
                    i, cp.position.x, cp.position.y, cp.position.z,
                    i, cp.radius,
                    i, cp.colorTint.x, cp.colorTint.y, cp.colorTint.z, cp.colorTint.w);
                ss << pb;
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            BeamComponent b;
            b.globalRadiusScale = GetF(kv, "radiusScale", b.globalRadiusScale);
            b.wobbleAmplitude   = GetF(kv, "wobbleAmp",   b.wobbleAmplitude);
            b.wobbleSpeed       = GetF(kv, "wobbleSpeed", b.wobbleSpeed);
            const int count = GetI(kv, "count", 0);
            b.controlPoints.reserve(static_cast<size_t>(count > 0 ? count : 0));
            for (int i = 0; i < count; ++i)
            {
                BeamControlPoint cp;
                char pk[16], rk[16], ck[16];
                snprintf(pk, sizeof(pk), "p%d", i);
                snprintf(rk, sizeof(rk), "r%d", i);
                snprintf(ck, sizeof(ck), "c%d", i);
                auto pit = kv.find(pk);
                if (pit != kv.end())
                    sscanf_s(pit->second.c_str(), "%f_%f_%f",
                             &cp.position.x, &cp.position.y, &cp.position.z);
                cp.radius = GetF(kv, rk, cp.radius);
                auto cit = kv.find(ck);
                if (cit != kv.end())
                    sscanf_s(cit->second.c_str(), "%f_%f_%f_%f",
                             &cp.colorTint.x, &cp.colorTint.y,
                             &cp.colorTint.z, &cp.colorTint.w);
                b.controlPoints.push_back(cp);
            }
            // beamSlot stays 0xFFFFFFFF so the Renderer allocates a fresh slot.
            w.AddComponent<BeamComponent>(e, std::move(b));
        }
    });

    // ==== Particle components (composition model) ====
    // A particle entity carries one ParticleRenderComponent (appearance) + one behaviour (Emitter /
    // Flock / Ambient). Runtime fields (bindless idx, spawnAccumulator, spawn latches) are re-derived
    // on load; a combined "ParticleEmitter:" line from an older scene migrates via
    // LegacyParticleEmitterTag.
    // Shared field readers, captured by value into the loaders and the migrator so every parse path
    // is identical.
    auto pv3 = [](const KVMap& kv, const char* k, XMFLOAT3& o) {
        auto it = kv.find(k); if (it != kv.end()) sscanf_s(it->second.c_str(), "%f_%f_%f", &o.x, &o.y, &o.z);
    };
    auto pv4 = [](const KVMap& kv, const char* k, XMFLOAT4& o) {
        auto it = kv.find(k); if (it != kv.end()) sscanf_s(it->second.c_str(), "%f_%f_%f_%f", &o.x, &o.y, &o.z, &o.w);
    };
    auto readRender = [pv3, pv4](const KVMap& kv, ParticleRenderComponent& p) {
        pv4(kv, "startColor", p.startColor); pv4(kv, "endColor", p.endColor);
        p.startSize     = GetF(kv, "startSize", p.startSize);
        pv3(kv, "startScale", p.startScale);
        p.intensity     = GetF(kv, "intensity", p.intensity);
        p.emissiveFade  = GetF(kv, "emissiveFade", p.emissiveFade);
        p.velocityStretch = GetF(kv, "velocityStretch", p.velocityStretch);
        p.blendMode   = static_cast<ParticleBlendMode>(GetI(kv, "blendMode", static_cast<int>(p.blendMode)));
        p.visualMode  = static_cast<ParticleVisualMode>(GetI(kv, "visualMode", static_cast<int>(p.visualMode)));
        p.orientation = static_cast<ParticleOrientation>(GetI(kv, "orientation", static_cast<int>(p.orientation)));
        p.excludeFromTAA = GetI(kv, "excludeTAA", p.excludeFromTAA ? 1 : 0) != 0;
        p.lit            = GetI(kv, "lit", 0) != 0;
        // Absent key = off, deliberately: lit emitters predate this key, and defaulting it on would
        // switch shadows on for all of them at once.
        p.receiveShadow  = GetI(kv, "particleReceiveShadow", 0) != 0;
        p.useCustomShader  = GetI(kv, "useCustomShader", 0) != 0;
        p.customVertexPath  = GetS(kv, "customVertexPath");
        p.customSurfacePath = GetS(kv, "customSurfacePath");
        p.texturePath       = GetS(kv, "texturePath");
        p.textureBindlessIdx = -1; p.textureGpuHandle = 0;
    };
    auto readEmit = [pv3](const KVMap& kv, ParticleEmitterComponent& p) {
        p.enabled       = GetI(kv, "enabled", 1) != 0;
        p.spawnRate     = GetF(kv, "spawnRate", p.spawnRate);
        p.maxParticles  = GetI(kv, "maxParticles", p.maxParticles);
        p.burstCount    = GetI(kv, "burstCount", p.burstCount);
        p.startLifetime = GetF(kv, "startLifetime", p.startLifetime);
        p.sizeVariance  = GetF(kv, "sizeVariance", p.sizeVariance);
        p.lifeVariance  = GetF(kv, "lifeVariance", p.lifeVariance);
        pv3(kv, "velMin", p.velocityMin); pv3(kv, "velMax", p.velocityMax);
        pv3(kv, "gravity", p.gravity);
        p.drag          = GetF(kv, "drag", p.drag);
        p.shape         = static_cast<ParticleShape>(GetI(kv, "shape", static_cast<int>(p.shape)));
        p.sphereRadius       = GetF(kv, "sphereRadius", p.sphereRadius);
        p.sphereSpawnOnShell = GetI(kv, "sphereShell", p.sphereSpawnOnShell ? 1 : 0) != 0;
        p.coneHalfAngle = GetF(kv, "coneHalfAngle", p.coneHalfAngle);
        pv3(kv, "coneDir", p.coneDirection);
        p.coneLength    = GetF(kv, "coneLength", p.coneLength);
        pv3(kv, "boxHalfExtents", p.boxHalfExtents);
        p.circleRadius  = GetF(kv, "circleRadius", p.circleRadius);
        pv3(kv, "circleNormal", p.circleNormal);
        p.meshSourceEntity = GetU(kv, "meshSourceEntity", p.meshSourceEntity);
        pv3(kv, "bladeTipLocal", p.bladeTipLocal);
        p.inheritVelocity = GetF(kv, "inheritVelocity", p.inheritVelocity);
        p.minSwingSpeed   = GetF(kv, "minSwingSpeed",   p.minSwingSpeed);
        p.sparksPerMeter  = GetF(kv, "sparksPerMeter",  p.sparksPerMeter);
    };
    auto readFlock = [](const KVMap& kv, ParticleFlockComponent& p) {
        p.enabled          = GetI(kv, "enabled", 1) != 0;
        p.flockCount       = GetI(kv, "flockCount", p.flockCount);
        p.perceptionRadius = GetF(kv, "perceptionRadius", p.perceptionRadius);
        p.separationRadius = GetF(kv, "separationRadius", p.separationRadius);
        p.separationWeight = GetF(kv, "separationWeight", p.separationWeight);
        p.alignmentWeight  = GetF(kv, "alignmentWeight", p.alignmentWeight);
        p.cohesionWeight   = GetF(kv, "cohesionWeight", p.cohesionWeight);
        p.maxSpeed         = GetF(kv, "maxSpeed", p.maxSpeed);
        p.maxForce         = GetF(kv, "maxForce", p.maxForce);
        p.boundsRadius     = GetF(kv, "boundsRadius", p.boundsRadius);
        p.boundsWeight     = GetF(kv, "boundsWeight", p.boundsWeight);
    };
    auto readAmbient = [pv3](const KVMap& kv, ParticleAmbientComponent& p) {
        p.enabled        = GetI(kv, "enabled", 1) != 0;
        p.ambientCount   = GetI(kv, "ambientCount", p.ambientCount);
        p.ambientDensity = GetF(kv, "ambientDensity", p.ambientDensity);
        pv3(kv, "ambientExtents", p.ambientExtents);
        pv3(kv, "ambientOffset", p.ambientOffset);
        pv3(kv, "ambientWind", p.ambientWind);
        p.ambientEdgeFade = GetF(kv, "ambientEdgeFade", p.ambientEdgeFade);
        { auto it = kv.find("ambientNearFade");
          if (it != kv.end()) sscanf_s(it->second.c_str(), "%f_%f", &p.ambientNearFade0, &p.ambientNearFade1); }
        pv3(kv, "velMin", p.velocityMin); pv3(kv, "velMax", p.velocityMax);
        p.drag          = GetF(kv, "drag", p.drag);
        p.startLifetime = GetF(kv, "startLifetime", p.startLifetime);
    };

    // -- ParticleRenderComponent (shared appearance) --
    reg.Register(std::type_index(typeid(ParticleRenderComponent)), {
        "ParticleRender",
        [](World& w, Entity e) { return w.GetComponent<ParticleRenderComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<ParticleRenderComponent>(e);
            if (!p) return;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "  ParticleRender: startColor=%.4f_%.4f_%.4f_%.4f endColor=%.4f_%.4f_%.4f_%.4f"
                " startSize=%.4f startScale=%.4f_%.4f_%.4f intensity=%.4f emissiveFade=%.4f velocityStretch=%.4f"
                " blendMode=%u visualMode=%u orientation=%u excludeTAA=%u lit=%u",
                p->startColor.x, p->startColor.y, p->startColor.z, p->startColor.w,
                p->endColor.x, p->endColor.y, p->endColor.z, p->endColor.w,
                p->startSize, p->startScale.x, p->startScale.y, p->startScale.z,
                p->intensity, p->emissiveFade, p->velocityStretch,
                static_cast<uint32_t>(p->blendMode), static_cast<uint32_t>(p->visualMode),
                static_cast<uint32_t>(p->orientation), p->excludeFromTAA ? 1u : 0u, p->lit ? 1u : 0u);
            ss << buf;
            // Written only when set (an absent key reads as off / empty), so a component without them writes
            // none of these keys.
            if (p->receiveShadow)   ss << " particleReceiveShadow=1";
            if (p->useCustomShader) ss << " useCustomShader=1";
            if (!p->customVertexPath.empty())  ss << " customVertexPath="  << PercentEncode(p->customVertexPath);
            if (!p->customSurfacePath.empty()) ss << " customSurfacePath=" << PercentEncode(p->customSurfacePath);
            if (!p->texturePath.empty())       ss << " texturePath="       << PercentEncode(p->texturePath);
            ss << "\n";
        },
        [readRender](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ParticleRenderComponent p; readRender(kv, p);
            w.AddComponent<ParticleRenderComponent>(e, p);
        }
    });

    // -- ParticleEmitterComponent (standard emit behaviour) --
    reg.Register(std::type_index(typeid(ParticleEmitterComponent)), {
        "ParticleEmit",
        [](World& w, Entity e) { return w.GetComponent<ParticleEmitterComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<ParticleEmitterComponent>(e);
            if (!p) return;
            // Sized with headroom: the worst case passes ~700 chars, and an overflowing snprintf truncates
            // silently (the scene then loads defaults for whatever fell off the end).
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "  ParticleEmit: enabled=%u spawnRate=%.4f maxParticles=%d burstCount=%d"
                " startLifetime=%.4f sizeVariance=%.4f lifeVariance=%.4f"
                " velMin=%.4f_%.4f_%.4f velMax=%.4f_%.4f_%.4f gravity=%.4f_%.4f_%.4f drag=%.4f"
                " shape=%u sphereRadius=%.4f sphereShell=%u"
                " coneHalfAngle=%.4f coneDir=%.4f_%.4f_%.4f coneLength=%.4f"
                " boxHalfExtents=%.4f_%.4f_%.4f circleRadius=%.4f circleNormal=%.4f_%.4f_%.4f"
                " meshSourceEntity=%u"
                " bladeTipLocal=%.4f_%.4f_%.4f inheritVelocity=%.4f minSwingSpeed=%.4f"
                " sparksPerMeter=%.4f",
                p->enabled ? 1u : 0u, p->spawnRate, p->maxParticles, p->burstCount,
                p->startLifetime, p->sizeVariance, p->lifeVariance,
                p->velocityMin.x, p->velocityMin.y, p->velocityMin.z,
                p->velocityMax.x, p->velocityMax.y, p->velocityMax.z,
                p->gravity.x, p->gravity.y, p->gravity.z, p->drag,
                static_cast<uint32_t>(p->shape), p->sphereRadius, p->sphereSpawnOnShell ? 1u : 0u,
                p->coneHalfAngle, p->coneDirection.x, p->coneDirection.y, p->coneDirection.z, p->coneLength,
                p->boxHalfExtents.x, p->boxHalfExtents.y, p->boxHalfExtents.z,
                p->circleRadius, p->circleNormal.x, p->circleNormal.y, p->circleNormal.z,
                static_cast<uint32_t>(p->meshSourceEntity),
                p->bladeTipLocal.x, p->bladeTipLocal.y, p->bladeTipLocal.z,
                p->inheritVelocity, p->minSwingSpeed, p->sparksPerMeter);
            ss << buf << "\n";
        },
        [readEmit](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ParticleEmitterComponent p; readEmit(kv, p);
            w.AddComponent<ParticleEmitterComponent>(e, p);
        }
    });

    // -- ParticleFlockComponent (Boids behaviour) --
    reg.Register(std::type_index(typeid(ParticleFlockComponent)), {
        "ParticleFlock",
        [](World& w, Entity e) { return w.GetComponent<ParticleFlockComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<ParticleFlockComponent>(e);
            if (!p) return;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "  ParticleFlock: enabled=%u flockCount=%d perceptionRadius=%.4f separationRadius=%.4f"
                " separationWeight=%.4f alignmentWeight=%.4f cohesionWeight=%.4f"
                " maxSpeed=%.4f maxForce=%.4f boundsRadius=%.4f boundsWeight=%.4f",
                p->enabled ? 1u : 0u, p->flockCount, p->perceptionRadius, p->separationRadius,
                p->separationWeight, p->alignmentWeight, p->cohesionWeight,
                p->maxSpeed, p->maxForce, p->boundsRadius, p->boundsWeight);
            ss << buf << "\n";
        },
        [readFlock](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ParticleFlockComponent p; readFlock(kv, p);
            w.AddComponent<ParticleFlockComponent>(e, p);
        }
    });

    // -- ParticleAmbientComponent (camera-follow wrap volume behaviour) --
    reg.Register(std::type_index(typeid(ParticleAmbientComponent)), {
        "ParticleAmbient",
        [](World& w, Entity e) { return w.GetComponent<ParticleAmbientComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<ParticleAmbientComponent>(e);
            if (!p) return;
            char buf[448];
            snprintf(buf, sizeof(buf),
                "  ParticleAmbient: enabled=%u ambientCount=%d ambientDensity=%.4f"
                " ambientExtents=%.4f_%.4f_%.4f ambientOffset=%.4f_%.4f_%.4f"
                " ambientEdgeFade=%.4f ambientWind=%.4f_%.4f_%.4f ambientNearFade=%.4f_%.4f"
                " velMin=%.4f_%.4f_%.4f velMax=%.4f_%.4f_%.4f drag=%.4f startLifetime=%.4f",
                p->enabled ? 1u : 0u, p->ambientCount, p->ambientDensity,
                p->ambientExtents.x, p->ambientExtents.y, p->ambientExtents.z,
                p->ambientOffset.x, p->ambientOffset.y, p->ambientOffset.z,
                p->ambientEdgeFade, p->ambientWind.x, p->ambientWind.y, p->ambientWind.z,
                p->ambientNearFade0, p->ambientNearFade1,
                p->velocityMin.x, p->velocityMin.y, p->velocityMin.z,
                p->velocityMax.x, p->velocityMax.y, p->velocityMax.z,
                p->drag, p->startLifetime);
            ss << buf << "\n";
        },
        [readAmbient](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ParticleAmbientComponent p; readAmbient(kv, p);
            w.AddComponent<ParticleAmbientComponent>(e, p);
        }
    });

    // -- Migrator: an older scene's combined "ParticleEmitter:" line -> Render + one behaviour --
    // Never saved (has() -> false); only its deserialize runs.
    reg.Register(std::type_index(typeid(LegacyParticleEmitterTag)), {
        "ParticleEmitter",
        [](World&, Entity) { return false; },
        [](World&, Entity, std::ostringstream&) {},
        [readRender, readEmit, readFlock, readAmbient](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ParticleRenderComponent rc; readRender(kv, rc);
            w.AddComponent<ParticleRenderComponent>(e, rc);
            if (GetI(kv, "flocking", 0) != 0)
            {
                ParticleFlockComponent fc; readFlock(kv, fc);
                w.AddComponent<ParticleFlockComponent>(e, fc);
            }
            else if (GetI(kv, "ambient", 0) != 0)
            {
                ParticleAmbientComponent ac; readAmbient(kv, ac);
                w.AddComponent<ParticleAmbientComponent>(e, ac);
            }
            else
            {
                ParticleEmitterComponent ec; readEmit(kv, ec);
                w.AddComponent<ParticleEmitterComponent>(e, ec);
            }
        }
    });

    // ==== TimelineComponent (INLINE, AnimNotify tracks) ====
    // The nested structure (tracks -> notifies + states -> PropertyBag) does not fit the flat KV
    // format, so the whole payload is one percent-encoded JSON blob under key "tl" (the KV-line parser
    // accepts any value without unencoded spaces or '='). Runtime CPU state (lastObservedTime) is reset
    // on load; TimelineSystem snapshots the current animation time on first sight.
    reg.Register(std::type_index(typeid(TimelineComponent)), {
        "Timeline",
        [](World& w, Entity e) {
            const auto* tl = w.GetComponent<TimelineComponent>(e);
            // Skip empty timelines so an editor-created shell does not bloat every scene file; the runtime
            // creates them on demand.
            return tl && !tl->tracks.empty();
        },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* tl = w.GetComponent<TimelineComponent>(e);
            if (!tl) return;
            // Schema is shared with the .ianim notify section (NotifyIO) so a
            // clip-authored notify set and an entity timeline are byte-for-byte
            // interchangeable. See ECS/NotifySerialization.h.
            const std::string payload =
                NotifyIO::TracksToJsonString(tl->tracks, tl->clipDuration, tl->nextNotifyId);
            ss << "  Timeline: tl=" << PercentEncode(payload) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            const auto it = kv.find("tl");
            if (it == kv.end()) return;

            TimelineComponent tl;
            // lastObservedTime stays -1 -> TimelineSystem snapshots on first sight without firing every
            // notify in the clip.
            if (!NotifyIO::TracksFromJsonString(PercentDecode(it->second),
                                                tl.tracks, &tl.clipDuration, &tl.nextNotifyId))
                return; // malformed payload: skip rather than add a broken component
            w.AddComponent<TimelineComponent>(e, std::move(tl));
        }
    });

    // ==== CameraComponent (INLINE, lens / view-projection only) ====
    // The pose lives on LocalTransform (N line) and the FPS-controller state on the separate
    // CameraController block.
    reg.Register(std::type_index(typeid(CameraComponent)), {
        "Camera",
        [](World& w, Entity e) { return w.GetComponent<CameraComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<CameraComponent>(e);
            if (!c) return;
            char buf[128];
            snprintf(buf, sizeof(buf),
                "  Camera: fov=%.4f nearZ=%.4f farZ=%.4f\n",
                c->fov, c->nearZ, c->farZ);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CameraComponent c;
            c.fov   = GetF(kv, "fov", 1.047f);
            c.nearZ = GetF(kv, "nearZ", 0.1f);
            c.farZ  = GetF(kv, "farZ", 200.f);
            w.AddComponent<CameraComponent>(e, c);

            // Migration: an older Camera line carries the pose + FPS-controller fields inline (px / yaw ...);
            // rebuild them as LocalTransform and a CameraController component.
            if (kv.count("px") || kv.count("yaw"))
            {
                if (!w.GetComponent<CameraControllerComponent>(e))
                {
                    CameraControllerComponent ctrl;
                    ctrl.yaw       = GetF(kv, "yaw",       -2.47f);
                    ctrl.pitch     = GetF(kv, "pitch",      0.44f);
                    ctrl.moveSpeed = GetF(kv, "moveSpeed", 10.f);
                    w.AddComponent<CameraControllerComponent>(e, ctrl);
                }
                if (auto* lt = w.GetComponent<LocalTransform>(e))
                {
                    lt->translation = { GetF(kv,"px",4.f), GetF(kv,"py",3.f), GetF(kv,"pz",5.f) };
                    XMStoreFloat4(&lt->rotation,
                        XMQuaternionRotationRollPitchYaw(
                            GetF(kv,"pitch",0.44f), GetF(kv,"yaw",-2.47f), 0.f));
                }
            }
        }
    });

    // ==== VirtualCameraComponent (INLINE) ====
    // The VCam's lens + channel binding; without it a cinematic shot reloads with the default 60-deg
    // Main-channel lens. The pose lives on CameraPose / LocalTransform.
    RegisterReflectedSerializer<VirtualCameraComponent>(reg, "VirtualCamera", Reflect::NumberStyle::Fixed4);

    // ==== CameraPoseComponent (INLINE) ====
    // Behavior systems overwrite this every frame, but a hand-placed cinematic VCam with no behavior
    // system has nothing else driving it: there the authored pose is the data.
    reg.Register(std::type_index(typeid(CameraPoseComponent)), {
        "CameraPose",
        [](World& w, Entity e) { return w.GetComponent<CameraPoseComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<CameraPoseComponent>(e);
            if (!p) return;
            char buf[192];
            snprintf(buf, sizeof(buf),
                "  CameraPose: pos=%.4f_%.4f_%.4f rot=%.6f_%.6f_%.6f_%.6f\n",
                p->position.x, p->position.y, p->position.z,
                p->rotation.x, p->rotation.y, p->rotation.z, p->rotation.w);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CameraPoseComponent p;
            auto pit = kv.find("pos");
            if (pit != kv.end())
                sscanf_s(pit->second.c_str(), "%f_%f_%f",
                         &p.position.x, &p.position.y, &p.position.z);
            auto rit = kv.find("rot");
            if (rit != kv.end())
                sscanf_s(rit->second.c_str(), "%f_%f_%f_%f",
                         &p.rotation.x, &p.rotation.y, &p.rotation.z, &p.rotation.w);
            w.AddComponent<CameraPoseComponent>(e, p);
        }
    });

    // ==== VCamPriorityComponent (INLINE) ====
    // Stack ordering: without it every VCam would reload at priority 0 and the winning camera would
    // depend on load order.
    RegisterReflectedSerializer<VCamPriorityComponent>(reg, "VCamPriority", Reflect::NumberStyle::Fixed4);

    // ==== VCamBlendComponent (INLINE) ====
    // Only the authored half is saved: durations + curves. currentBlend / state are the runtime state
    // machine CameraStackSystem rebuilds on the first tick; persisting them would resurrect a
    // half-finished blend on load.
    reg.Register(std::type_index(typeid(VCamBlendComponent)), {
        "VCamBlend",
        [](World& w, Entity e) { return w.GetComponent<VCamBlendComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* b = w.GetComponent<VCamBlendComponent>(e);
            if (!b) return;
            char buf[160];
            snprintf(buf, sizeof(buf),
                "  VCamBlend: inDur=%.4f outDur=%.4f curveIn=%u curveOut=%u\n",
                b->blendInDuration, b->blendOutDuration,
                static_cast<uint32_t>(b->curveIn), static_cast<uint32_t>(b->curveOut));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            VCamBlendComponent b;
            b.blendInDuration  = GetF(kv, "inDur",  b.blendInDuration);
            b.blendOutDuration = GetF(kv, "outDur", b.blendOutDuration);
            b.curveIn  = static_cast<BlendCurve>(GetI(kv, "curveIn",
                             static_cast<int>(b.curveIn)));
            b.curveOut = static_cast<BlendCurve>(GetI(kv, "curveOut",
                             static_cast<int>(b.curveOut)));
            // currentBlend / state stay at their Inactive defaults on purpose.
            w.AddComponent<VCamBlendComponent>(e, b);
        }
    });

    // ==== CameraShakeComponent (INLINE) ====
    // Shake tuning is authored data; `trauma` and `time` are the live impulse and its phase
    // accumulator, dropped so a saved scene does not reopen mid-shake.
    reg.Register(std::type_index(typeid(CameraShakeComponent)), {
        "CameraShake",
        [](World& w, Entity e) { return w.GetComponent<CameraShakeComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* s = w.GetComponent<CameraShakeComponent>(e);
            if (!s) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  CameraShake: falloff=%.4f posAmp=%.4f_%.4f_%.4f"
                " rotAmp=%.4f_%.4f_%.4f frequency=%.4f seed=%u\n",
                s->falloffPerSec,
                s->posAmplitude.x, s->posAmplitude.y, s->posAmplitude.z,
                s->rotAmplitude.x, s->rotAmplitude.y, s->rotAmplitude.z,
                s->frequency, s->seed);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CameraShakeComponent s;
            s.falloffPerSec = GetF(kv, "falloff", s.falloffPerSec);
            auto pit = kv.find("posAmp");
            if (pit != kv.end())
                sscanf_s(pit->second.c_str(), "%f_%f_%f",
                         &s.posAmplitude.x, &s.posAmplitude.y, &s.posAmplitude.z);
            auto rit = kv.find("rotAmp");
            if (rit != kv.end())
                sscanf_s(rit->second.c_str(), "%f_%f_%f",
                         &s.rotAmplitude.x, &s.rotAmplitude.y, &s.rotAmplitude.z);
            s.frequency = GetF(kv, "frequency", s.frequency);
            s.seed      = GetU(kv, "seed", s.seed);
            // trauma / time stay 0: the scene opens calm.
            w.AddComponent<CameraShakeComponent>(e, s);
        }
    });

    // ==== CameraMotionComponent (plays a camera .ianim onto a VCam) ====
    reg.Register(std::type_index(typeid(CameraMotionComponent)), {
        "CameraMotion",
        [](World& w, Entity e) { return w.GetComponent<CameraMotionComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<CameraMotionComponent>(e);
            if (!c) return;
            ss << "  CameraMotion: path=" << PercentEncode(c->motionPath)
               << " speed="      << c->speed
               << " translationScale=" << c->translationScale
               << " looping="    << (c->looping    ? 1 : 0)
               << " playOnLoad=" << (c->playOnLoad ? 1 : 0)
               << " priority="   << c->priority
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CameraMotionComponent c;
            c.motionPath = GetS(kv, "path", "");
            c.speed      = GetF(kv, "speed", 1.f);
            c.translationScale = GetF(kv, "translationScale", 1.f);
            c.looping    = GetI(kv, "looping",    0) != 0;
            c.playOnLoad = GetI(kv, "playOnLoad", 1) != 0;
            c.priority   = GetI(kv, "priority",   1000);
            w.AddComponent<CameraMotionComponent>(e, std::move(c));
        }
    });

    // ==== MotionDirectorComponent (master clock syncing N motion tracks) ====
    reg.Register(std::type_index(typeid(MotionDirectorComponent)), {
        "MotionDirector",
        [](World& w, Entity e) { return w.GetComponent<MotionDirectorComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* d = w.GetComponent<MotionDirectorComponent>(e);
            if (!d) return;
            ss << "  MotionDirector: speed=" << d->masterSpeed
               << " duration="   << d->duration
               << " looping="    << (d->looping    ? 1 : 0)
               << " playOnLoad=" << (d->playOnLoad ? 1 : 0)
               << " count="      << d->tracks.size();
            for (size_t i = 0; i < d->tracks.size(); ++i)
            {
                const MotionDirectorTrack& tr = d->tracks[i];
                char pfx[16]; snprintf(pfx, sizeof(pfx), "t%zu", i);
                WriteAttachmentRef(ss, pfx, w, tr.target);   // " t<i>Guid=<hex>"
                ss << " ty"  << i << "=" << static_cast<int>(tr.type)
                   << " off" << i << "=" << tr.timeOffset
                   << " spd" << i << "=" << tr.speedMul;
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            MotionDirectorComponent d;
            d.masterSpeed = GetF(kv, "speed",      1.f);
            d.duration    = GetF(kv, "duration",   0.f);
            d.looping     = GetI(kv, "looping",    0) != 0;
            d.playOnLoad  = GetI(kv, "playOnLoad", 1) != 0;
            const int count = GetI(kv, "count", 0);
            d.tracks.reserve(static_cast<size_t>(count > 0 ? count : 0));
            for (int i = 0; i < count; ++i)
            {
                MotionDirectorTrack tr;
                char pfx[16], tyk[16], offk[16], spdk[16];
                snprintf(pfx,  sizeof(pfx),  "t%d",   i);
                snprintf(tyk,  sizeof(tyk),  "ty%d",  i);
                snprintf(offk, sizeof(offk), "off%d", i);
                snprintf(spdk, sizeof(spdk), "spd%d", i);
                ReadAttachmentRef(kv, pfx, nullptr, w, e, tr.target);
                tr.type       = static_cast<uint8_t>(GetI(kv, tyk, 0));
                tr.timeOffset = GetF(kv, offk, 0.f);
                tr.speedMul   = GetF(kv, spdk, 1.f);
                d.tracks.push_back(tr);
            }
            w.AddComponent<MotionDirectorComponent>(e, std::move(d));
        }
    });

    // ==== SequencePlayerComponent (cinematic sequence playback) ====
    // The cast is serialized here, not in the .iseq: the asset declares roles, the placement supplies
    // the actors, so one cutscene can run in two levels or on two prefab instances. Of the transport
    // state only frame / playing go to disk (for save games); ComponentCloner<SequencePlayerComponent>
    // drops all of it on an Edit <-> Play snapshot.
    reg.Register(std::type_index(typeid(SequencePlayerComponent)), {
        "SequencePlayer",
        [](World& w, Entity e) { return w.GetComponent<SequencePlayerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<SequencePlayerComponent>(e);
            if (!p) return;
            ss << "  SequencePlayer: path=" << PercentEncode(p->sequencePath)
               << " playOnLoad="  << (p->playOnLoad      ? 1 : 0)
               << " ovrLoop="     << (p->overrideLooping ? 1 : 0)
               << " looping="     << (p->looping         ? 1 : 0)
               << " rate="        << p->rate
               << " flow="        << PercentEncode(p->gameFlowState)
               << " skip="        << static_cast<int>(p->skipPolicy)
               << " watched="     << (p->recordWatched ? 1 : 0)
               // Runtime, but save-game relevant: a save taken mid-cutscene reloads where it was instead of
               // replaying from the top. A scene save writes playing=0 / frame=0, since the authoring world is
               // never mid-playback.
               << " frame="       << p->frame
               << " playing="     << (p->playing ? 1 : 0)
               << " count="       << p->roleBindings.size();
            for (size_t i = 0; i < p->roleBindings.size(); ++i)
            {
                const SequenceRoleBinding& rb = p->roleBindings[i];
                char pfx[16]; snprintf(pfx, sizeof(pfx), "b%zu", i);
                WriteAttachmentRef(ss, pfx, w, rb.target);   // " b<i>Guid=<hex>"
                ss << " r" << i << "=" << PercentEncode(rb.role);
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            SequencePlayerComponent p;
            p.sequencePath    = GetS(kv, "path", "");
            p.playOnLoad      = GetI(kv, "playOnLoad", 0) != 0;
            p.overrideLooping = GetI(kv, "ovrLoop",    0) != 0;
            p.looping         = GetI(kv, "looping",    0) != 0;
            p.rate            = GetF(kv, "rate",       1.f);
            p.gameFlowState   = GetS(kv, "flow",  "");
            // "inctx" is not read: the input context belongs to the cutscene's GameFlow state, not to the
            // player. An old scene's key is ignored like any unknown key.
            p.skipPolicy      = static_cast<SequencePlayerComponent::SkipPolicy>(
                                    GetI(kv, "skip", 1));
            p.recordWatched   = GetI(kv, "watched", 0) != 0;
            p.frame           = GetI(kv, "frame",   0);
            // `started` is set with `playing` so a resumed save does not also
            // re-fire playOnLoad on top of the restored playback.
            p.playing         = GetI(kv, "playing", 0) != 0;
            p.started         = p.playing;
            p.prevFrame       = -1;   // resumed playback is a discontinuity
            const int count = GetI(kv, "count", 0);
            p.roleBindings.reserve(static_cast<size_t>(count > 0 ? count : 0));
            for (int i = 0; i < count; ++i)
            {
                SequenceRoleBinding rb;
                char pfx[16], rk[16];
                snprintf(pfx, sizeof(pfx), "b%d", i);
                snprintf(rk,  sizeof(rk),  "r%d", i);
                ReadAttachmentRef(kv, pfx, nullptr, w, e, rb.target);
                rb.role = GetS(kv, rk, "");
                p.roleBindings.push_back(std::move(rb));
            }
            w.AddComponent<SequencePlayerComponent>(e, std::move(p));
        }
    });

    // ==== AudioSourceComponent (3D / 2D sound emitter) ====
    reg.Register(std::type_index(typeid(Audio::AudioSourceComponent)), {
        "AudioSource",
        [](World& w, Entity e) { return w.GetComponent<Audio::AudioSourceComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* a = w.GetComponent<Audio::AudioSourceComponent>(e);
            if (!a) return;
            ss << "  AudioSource: clip=" << PercentEncode(a->clipPath)
               << " bus="          << static_cast<int>(a->bus)
               << " volume="       << a->volume
               << " pitch="        << a->pitch
               << " loop="         << (a->loop         ? 1 : 0)
               << " is3D="         << (a->is3D         ? 1 : 0)
               << " playOnEnable=" << (a->playOnEnable ? 1 : 0)
               << " minDist="      << a->minDistance
               << " maxDist="      << a->maxDistance
               << " curve="        << static_cast<int>(a->curve)
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            Audio::AudioSourceComponent a;
            a.clipPath     = GetS(kv, "clip", "");
            a.bus          = static_cast<Audio::BusType>(
                                 GetI(kv, "bus", static_cast<int>(Audio::BusType::SFX)));
            a.volume       = GetF(kv, "volume", 1.f);
            a.pitch        = GetF(kv, "pitch",  1.f);
            a.loop         = GetI(kv, "loop",         0) != 0;
            a.is3D         = GetI(kv, "is3D",         1) != 0;
            a.playOnEnable = GetI(kv, "playOnEnable", 0) != 0;
            a.minDistance  = GetF(kv, "minDist",  1.f);
            a.maxDistance  = GetF(kv, "maxDist", 50.f);
            a.curve        = static_cast<Audio::AttenuationCurve>(GetI(kv, "curve", 0));
            w.AddComponent<Audio::AudioSourceComponent>(e, std::move(a));
        }
    });

    // ==== AudioListenerComponent (X3DAudio listener pose source) ====
    reg.Register(std::type_index(typeid(Audio::AudioListenerComponent)), {
        "AudioListener",
        [](World& w, Entity e) { return w.GetComponent<Audio::AudioListenerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* l = w.GetComponent<Audio::AudioListenerComponent>(e);
            if (!l) return;
            ss << "  AudioListener: masterVolume=" << l->masterVolume
               << " active=" << (l->active ? 1 : 0)
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            Audio::AudioListenerComponent l;
            l.masterVolume = GetF(kv, "masterVolume", 1.f);
            l.active       = GetI(kv, "active", 1) != 0;
            w.AddComponent<Audio::AudioListenerComponent>(e, std::move(l));
        }
    });

    // ==== GuidComponent (INLINE, stable cross-session identity) ====
    // Must load before any component that references this entity by GUID, so the target's GUID is on
    // it when a load reaches an AttachmentRef. SceneSerializer runs the component blocks in file order;
    // the GuidRegistry is also rebuilt from the world after the deserialize phase, a safety net for any
    // order.
    reg.Register(std::type_index(typeid(GuidComponent)), {
        "Guid",
        [](World& w, Entity e) { return w.GetComponent<GuidComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* gc = w.GetComponent<GuidComponent>(e);
            if (!gc || !gc->guid.IsValid()) return;
            ss << "  Guid: hex=" << gc->guid.ToHex() << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            GuidComponent gc{};
            auto it = kv.find("hex");
            if (it != kv.end()) gc.guid = ECS::Guid::FromHex(it->second);
            if (!gc.guid.IsValid()) gc.guid = ECS::Guid::Generate();
            w.AddComponent<GuidComponent>(e, gc);
            // Register now, so a later component in the same load pass can resolve a ref to this entity
            // before RebuildFromWorld runs.
            ECS::GuidRegistry::Get().Register(gc.guid, e);
        }
    });

    // ==== FollowSocketComponent / FollowEntityComponent ====
    // Attachments (a weapon in a hand socket, an HP bar over a head) must survive SaveScene, not only a
    // .prefab round trip. Both targets are AttachmentRefs: the Guid is the on-disk truth and
    // FollowSystem resolves it lazily; a follower deserialized before its target waits a frame.
    reg.Register(std::type_index(typeid(FollowSocketComponent)), {
        "FollowSocket",
        [](World& w, Entity e) { return w.GetComponent<FollowSocketComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            auto* fs = w.GetComponent<FollowSocketComponent>(e);
            if (!fs) return;
            // Stamp the target's Guid (idempotent) so the edge round-trips even
            // when the ref was bound at runtime and never went through the editor.
            if (const Entity t = fs->target.Resolve(w); t != NullEntity)
                fs->target.ownerGuid = ECS::EnsureGuidOn(w, t);
            // socketHash is the authority; socket= is a fallback index for refs
            // that address by raw index (VFX bone path).
            ss << "  FollowSocket: socket=" << fs->socketIndex
               << " socketHash=" << fs->socketNameHash;
            WriteAttachmentRef(ss, "target", w, fs->target);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            FollowSocketComponent fs;
            fs.socketIndex    = GetU(kv, "socket", 0u);
            fs.socketNameHash = GetU(kv, "socketHash", 0u);
            // cachedSocketGen stays 0 so the first tick re-resolves the index.
            ReadAttachmentRef(kv, "target", nullptr, w, e, fs.target);
            w.AddComponent<FollowSocketComponent>(e, fs);
        }
    });

    reg.Register(std::type_index(typeid(FollowEntityComponent)), {
        "FollowEntity",
        [](World& w, Entity e) { return w.GetComponent<FollowEntityComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            auto* fe = w.GetComponent<FollowEntityComponent>(e);
            if (!fe) return;
            if (const Entity t = fe->target.Resolve(w); t != NullEntity)
                fe->target.ownerGuid = ECS::EnsureGuidOn(w, t);
            ss << "  FollowEntity:";
            WriteAttachmentRef(ss, "target", w, fe->target);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            FollowEntityComponent fe;
            ReadAttachmentRef(kv, "target", nullptr, w, e, fe.target);
            w.AddComponent<FollowEntityComponent>(e, fe);
        }
    });

    // ==== PersistentTag (INLINE, save-game opt-in marker) ====
    // Empty body; presence marks the entity for the save file (SaveGame) in addition to the scene.
    reg.Register(std::type_index(typeid(PersistentTag)), {
        "Persistent",
        [](World& w, Entity e) { return w.GetComponent<PersistentTag>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            if (w.GetComponent<PersistentTag>(e))
                ss << "  Persistent:\n";
        },
        [](World& w, Entity e, const KVMap& /*kv*/, Resource::AssetManager*) {
            if (!w.HasComponent<PersistentTag>(e))
                w.AddComponent<PersistentTag>(e, PersistentTag{});
        }
    });

    // ==== CameraControllerComponent (INLINE, FPS controller state) ====
    // followTarget is written as a GUID (followTargetGuid=...); an older raw "followTarget=42" is read
    // via SceneLoadContext.
    reg.Register(std::type_index(typeid(CameraControllerComponent)), {
        "CameraController",
        [](World& w, Entity e) { return w.GetComponent<CameraControllerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<CameraControllerComponent>(e);
            if (!c) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  CameraController: yaw=%.4f pitch=%.4f"
                " mouseSensitivity=%.5f moveSpeed=%.4f mode=%d"
                " tpDist=%.3f hx=%.3f hy=%.3f hz=%.3f"
                " sx=%.3f sy=%.3f sz=%.3f camCol=%d camProbe=%.3f"
                " fLag=%.4f dLag=%.4f",
                c->yaw, c->pitch, c->mouseSensitivity, c->moveSpeed,
                (int)c->mode,
                c->thirdPersonDistance,
                c->headOffset.x, c->headOffset.y, c->headOffset.z,
                c->shoulderOffset.x, c->shoulderOffset.y, c->shoulderOffset.z,
                (int)c->cameraCollisionEnabled, c->cameraProbeRadius,
                c->followLag, c->distanceLag);
            ss << buf;
            WriteAttachmentRef(ss, "followTarget", w, c->followTarget);
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CameraControllerComponent c;
            c.yaw              = GetF(kv, "yaw",              -2.47f);
            c.pitch            = GetF(kv, "pitch",             0.44f);
            c.mouseSensitivity = GetF(kv, "mouseSensitivity",  0.003f);
            c.moveSpeed        = GetF(kv, "moveSpeed",        10.f);
            c.mode             = static_cast<CameraControllerComponent::Mode>(
                                     GetI(kv, "mode", (int)CameraControllerComponent::Mode::Free));
            c.thirdPersonDistance     = GetF(kv, "tpDist",   4.f);
            c.headOffset              = { GetF(kv,"hx",0.f), GetF(kv,"hy",1.65f), GetF(kv,"hz",0.f) };
            c.shoulderOffset          = { GetF(kv,"sx",0.30f), GetF(kv,"sy",1.60f), GetF(kv,"sz",0.f) };
            c.cameraCollisionEnabled  = GetI(kv, "camCol",   1) != 0;
            c.cameraProbeRadius       = GetF(kv, "camProbe", 0.20f);
            c.followLag               = GetF(kv, "fLag",     0.0f);
            c.distanceLag             = GetF(kv, "dLag",     0.0f);
            ReadAttachmentRef(kv, "followTarget", "followTarget", w, e, c.followTarget);
            w.AddComponent<CameraControllerComponent>(e, c);
        }
    });

    // ==== FollowCameraComponent (INLINE, camera-stack VCam behavior) ====
    // Drives a VCam to chase `target`, an AttachmentRef so it survives scene reload.
    reg.Register(std::type_index(typeid(FollowCameraComponent)), {
        "FollowCamera",
        [](World& w, Entity e) { return w.GetComponent<FollowCameraComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* fc = w.GetComponent<FollowCameraComponent>(e);
            if (!fc) return;
            char buf[224];
            snprintf(buf, sizeof(buf),
                "  FollowCamera: ox=%.3f oy=%.3f oz=%.3f"
                " lx=%.3f ly=%.3f lz=%.3f damp=%.3f rdamp=%.3f lookAt=%d",
                fc->offset.x, fc->offset.y, fc->offset.z,
                fc->lookAtOffset.x, fc->lookAtOffset.y, fc->lookAtOffset.z,
                fc->damping, fc->rotationDamping, (int)fc->useLookAt);
            ss << buf;
            WriteAttachmentRef(ss, "target", w, fc->target);
            ss << '\n';
            // The Follow tick system drives this VCam only with a FollowCameraTag: write the tag's presence
            // alongside.
            if (w.HasComponent<FollowCameraTag>(e))
                ss << "  FollowCameraTag:\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            FollowCameraComponent fc{};
            fc.offset          = { GetF(kv,"ox",0.f),  GetF(kv,"oy",1.6f),  GetF(kv,"oz",-4.f) };
            fc.lookAtOffset    = { GetF(kv,"lx",0.f),  GetF(kv,"ly",1.5f),  GetF(kv,"lz",0.f) };
            fc.damping         = GetF(kv, "damp",    8.f);
            fc.rotationDamping = GetF(kv, "rdamp",   8.f);
            fc.useLookAt       = GetI(kv, "lookAt",  1) != 0;
            ReadAttachmentRef(kv, "target", "target", w, e, fc.target);
            w.AddComponent<FollowCameraComponent>(e, fc);
        }
    });

    // FollowCameraTag: an empty marker, saved as presence only.
    reg.Register(std::type_index(typeid(FollowCameraTag)), {
        "FollowCameraTag",
        [](World& w, Entity e) { return w.GetComponent<FollowCameraTag>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            if (w.GetComponent<FollowCameraTag>(e))
                ss << "  FollowCameraTag:\n";
        },
        [](World& w, Entity e, const KVMap& /*kv*/, Resource::AssetManager*) {
            if (!w.HasComponent<FollowCameraTag>(e))
                w.AddComponent<FollowCameraTag>(e, FollowCameraTag{});
        }
    });

    // ==== AimCameraComponent (INLINE, third-person orbit VCam) ====
    reg.Register(std::type_index(typeid(AimCameraComponent)), {
        "AimCamera",
        [](World& w, Entity e) { return w.GetComponent<AimCameraComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ac = w.GetComponent<AimCameraComponent>(e);
            if (!ac) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  AimCamera: px=%.3f py=%.3f pz=%.3f"
                " yaw=%.4f pitch=%.4f dist=%.3f pMin=%.4f pMax=%.4f"
                " col=%d probe=%.3f",
                ac->pivotOffset.x, ac->pivotOffset.y, ac->pivotOffset.z,
                ac->yaw, ac->pitch, ac->distance, ac->pitchMin, ac->pitchMax,
                (int)ac->collisionAvoid, ac->probeRadius);
            ss << buf;
            WriteAttachmentRef(ss, "target", w, ac->target);
            ss << '\n';
            if (w.HasComponent<AimCameraTag>(e))
                ss << "  AimCameraTag:\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AimCameraComponent ac{};
            ac.pivotOffset    = { GetF(kv,"px",0.f), GetF(kv,"py",1.5f), GetF(kv,"pz",0.f) };
            ac.yaw            = GetF(kv,"yaw",       0.f);
            ac.pitch          = GetF(kv,"pitch",     0.f);
            ac.distance       = GetF(kv,"dist",      3.5f);
            ac.pitchMin       = GetF(kv,"pMin",     -1.4f);
            ac.pitchMax       = GetF(kv,"pMax",      1.0f);
            ac.collisionAvoid = GetI(kv,"col",       1) != 0;
            ac.probeRadius    = GetF(kv,"probe",     0.20f);
            ReadAttachmentRef(kv, "target", "target", w, e, ac.target);
            w.AddComponent<AimCameraComponent>(e, ac);
        }
    });

    reg.Register(std::type_index(typeid(AimCameraTag)), {
        "AimCameraTag",
        [](World& w, Entity e) { return w.GetComponent<AimCameraTag>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            if (w.GetComponent<AimCameraTag>(e))
                ss << "  AimCameraTag:\n";
        },
        [](World& w, Entity e, const KVMap& /*kv*/, Resource::AssetManager*) {
            if (!w.HasComponent<AimCameraTag>(e))
                w.AddComponent<AimCameraTag>(e, AimCameraTag{});
        }
    });

    // ==== EditorFolderComponent (INLINE, editor outliner folder path) ====
    // Editor-only organizational metadata with no runtime effect. Not reflected (no inspector panel):
    // it travels with the entity only to persist folder membership.
    reg.Register(std::type_index(typeid(EditorFolderComponent)), {
        "EdFolder",
        [](World& w, Entity e) { return w.GetComponent<EditorFolderComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* f = w.GetComponent<EditorFolderComponent>(e);
            if (!f || f->path.empty()) return;
            ss << "  EdFolder: path=" << PercentEncode(f->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            EditorFolderComponent f;
            f.path = GetS(kv, "path");
            if (!f.path.empty())
                w.AddComponent<EditorFolderComponent>(e, f);
        }
    });

    // ==== WorldEnvironmentComponent (INLINE, per-scene sky-mode singleton) ====
    // Its presence marks a scene authored under the optional-sky model: Scene::MigrateEnvironment
    // synthesizes one for an older scene that has none.
    reg.Register(std::type_index(typeid(WorldEnvironmentComponent)), {
        "WorldEnv",
        [](World& w, Entity e) { return w.GetComponent<WorldEnvironmentComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* v = w.GetComponent<WorldEnvironmentComponent>(e);
            if (!v) return;
            ss << "  WorldEnv: mode=" << static_cast<int>(v->mode)
               << " flatR=" << v->flatColor.x << " flatG=" << v->flatColor.y << " flatB=" << v->flatColor.z
               << " ambR="  << v->ambientColor.x << " ambG=" << v->ambientColor.y << " ambB=" << v->ambientColor.z
               << " ambI="  << v->ambientIntensity
               << " tod="   << (v->enableTimeOfDay ? 1 : 0)
               << " ibl="   << v->iblStrength
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            WorldEnvironmentComponent v;
            v.mode = static_cast<WorldEnvironmentComponent::Mode>(
                         GetI(kv, "mode", (int)WorldEnvironmentComponent::Mode::ProceduralAtmosphere));
            v.flatColor        = { GetF(kv, "flatR", 0.f), GetF(kv, "flatG", 0.f), GetF(kv, "flatB", 0.f) };
            v.ambientColor     = { GetF(kv, "ambR",  0.f), GetF(kv, "ambG",  0.f), GetF(kv, "ambB",  0.f) };
            v.ambientIntensity = GetF(kv, "ambI", 1.f);
            v.enableTimeOfDay  = GetI(kv, "tod", 0) != 0;
            // The sky light's intensity: an absent key (older scenes) stays at the -1 "inherit" sentinel, so
            // the AtmosphereComponent value keeps winning (see WorldEnvironment.h). An old `skyDif` key (no
            // such field) is not read: an unused KVMap entry.
            v.iblStrength        = GetF(kv, "ibl",    -1.f);
            w.AddComponent<WorldEnvironmentComponent>(e, v);
        }
    });

    // ==== SkyboxComponent (INLINE, its paths are asset refs) ====
    reg.Register(std::type_index(typeid(SkyboxComponent)), {
        "Skybox",
        [](World& w, Entity e) { return w.GetComponent<SkyboxComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* s = w.GetComponent<SkyboxComponent>(e);
            if (!s) return;
            ss << "  Skybox: irradiance=" << PercentEncode(s->irradiancePath)
               << " radiance=" << PercentEncode(s->radiancePath)
               << " skybox=" << PercentEncode(s->skyboxPath)
               << " material=" << PercentEncode(s->materialPath)
               << " baseMaterial=" << PercentEncode(s->baseMaterialPath)
               << " cloudMaterial=" << PercentEncode(s->cloudMaterialPath)
               << " mips=" << s->radianceMipLevels
               << " strength=" << s->iblStrength << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            SkyboxComponent s;
            s.irradiancePath    = GetS(kv, "irradiance");
            s.radiancePath      = GetS(kv, "radiance");
            s.skyboxPath        = GetS(kv, "skybox");
            s.materialPath      = GetS(kv, "material");
            s.baseMaterialPath  = GetS(kv, "baseMaterial");
            s.cloudMaterialPath = GetS(kv, "cloudMaterial");
            s.radianceMipLevels = static_cast<uint32_t>(GetI(kv, "mips", 7));
            s.iblStrength       = GetF(kv, "strength", 1.f);
            w.AddComponent<SkyboxComponent>(e, s);
        }
    });

    // ==== AtmosphereComponent (INLINE, atmospheric / IBL / aerial / star knobs) ====
    reg.Register(std::type_index(typeid(AtmosphereComponent)), {
        "Atmosphere",
        [](World& w, Entity e) { return w.GetComponent<AtmosphereComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* a = w.GetComponent<AtmosphereComponent>(e);
            if (!a) return;
            ss << "  Atmosphere: enabled=" << (a->atmosphereEnabled ? 1 : 0)
               << " source="  << static_cast<int>(a->skyboxSource)
               << " ibl=" << a->iblStrength
               << " aerial=" << (a->aerialCompositeEnabled ? 1 : 0)
               << " sunTrans=" << (a->sunTransmittanceEnabled ? 1 : 0)
               << " sunTransStr=" << a->sunTransmittanceStrength
               << " starDensity=" << a->starDensity
               << " starBright=" << a->starBrightness
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AtmosphereComponent a;
            a.atmosphereEnabled      = GetI(kv, "enabled", 1) != 0;
            a.skyboxSource           = static_cast<AtmosphereComponent::SkyboxSource>(
                                          GetI(kv, "source", 0));
            a.iblStrength            = GetF(kv, "ibl",      0.1f);
            a.aerialCompositeEnabled = GetI(kv, "aerial", 0) != 0;
            // Absent -> 1: matching UE is the point of the feature, so older scenes opt in on load.
            a.sunTransmittanceEnabled  = GetI(kv, "sunTrans", 1) != 0;
            a.sunTransmittanceStrength = GetF(kv, "sunTransStr", 1.f);
            a.starDensity            = GetF(kv, "starDensity",  256.f);
            a.starBrightness         = GetF(kv, "starBright",   0.7f);
            w.AddComponent<AtmosphereComponent>(e, a);
        }
    });

    // ==== TODConfigComponent (INLINE, authored Time-of-Day settings) ====
    reg.Register(std::type_index(typeid(TODConfigComponent)), {
        "TODConfig",
        [](World& w, Entity e) { return w.GetComponent<TODConfigComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<TODConfigComponent>(e);
            if (!c) return;
            ss << "  TODConfig: enabled=" << (c->enabled ? 1 : 0)
               << " t=" << c->timeOfDay
               << " speed=" << c->timeSpeed
               << " lat=" << c->latitudeRad
               << " sunScale=" << c->sunBrightnessScale
               << " moon=" << c->moonIntensityScale
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            TODConfigComponent c;
            c.enabled            = GetI(kv, "enabled", 0) != 0;
            c.timeOfDay          = GetF(kv, "t",     0.35f);
            c.timeSpeed          = GetF(kv, "speed", 0.f);
            c.latitudeRad        = GetF(kv, "lat",   0.6f);
            c.sunBrightnessScale = GetF(kv, "sunScale", 1.f);
            c.moonIntensityScale = GetF(kv, "moon",  0.015f);
            w.AddComponent<TODConfigComponent>(e, c);
            // TODOutputComponent is recomputed at runtime and not serialized, but TODEvaluationSystem
            // early-returns without one: re-attach it so a deserialized TODConfig always runs.
            if (!w.GetComponent<TODOutputComponent>(e))
                w.AddComponent<TODOutputComponent>(e, TODOutputComponent{});
        }
    });

    // ==== TODLightComponent (INLINE, a local light switched by the sun) ====
    // `lamp` / `bulbMat` are a name and a material substring, not asset paths: the target meshes are
    // .iscn-spawned (no stable guid to reference) and every mesh child of a node shares one name, so
    // the material is the only thing that separates a lamp's bulb from its pole.
    reg.Register(std::type_index(typeid(TODLightComponent)), {
        "TODLight",
        [](World& w, Entity e) { return w.GetComponent<TODLightComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<TODLightComponent>(e);
            if (!c) return;
            ss << "  TODLight: enabled=" << (c->enabled ? 1 : 0)
               << " on="        << c->onIntensity
               << " emOn="      << c->emissiveOn
               << " emOff="     << c->emissiveOff
               << " duskStart=" << c->duskStart
               << " duskEnd="   << c->duskEnd
               << " warmup="    << c->warmupTime;
            // Empty values are dropped by the parser, so only emit what is set;
            // the deserializer's defaults cover the rest.
            if (!c->lampName.empty())     ss << " lamp="    << PercentEncode(c->lampName);
            if (!c->bulbMaterial.empty()) ss << " bulbMat=" << PercentEncode(c->bulbMaterial);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            TODLightComponent c;
            c.enabled      = GetI(kv, "enabled", 1) != 0;
            c.onIntensity  = GetF(kv, "on",        25.f);
            c.emissiveOn   = GetF(kv, "emOn",       8.f);
            c.emissiveOff  = GetF(kv, "emOff",      0.f);
            c.duskStart    = GetF(kv, "duskStart",  0.12f);
            c.duskEnd      = GetF(kv, "duskEnd",   -0.05f);
            c.warmupTime   = GetF(kv, "warmup",     1.5f);
            // GetS already percent-decodes: do not wrap it in PercentDecode.
            c.lampName     = GetS(kv, "lamp");
            c.bulbMaterial = GetS(kv, "bulbMat");
            w.AddComponent<TODLightComponent>(e, c);
        }
    });

    // ==== CloudComponent (INLINE, volumetric cloud tunables) ====
    reg.Register(std::type_index(typeid(CloudComponent)), {
        "Clouds",
        [](World& w, Entity e) { return w.GetComponent<CloudComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<CloudComponent>(e);
            if (!c) return;
            ss << "  Clouds: enabled=" << (c->enabled ? 1 : 0)
               << " bottom=" << c->bottomAltitude
               << " top=" << c->topAltitude
               << " coverage=" << c->coverage
               << " density=" << c->density
               << " noiseScale=" << c->noiseScale
               << " detailScale=" << c->detailNoiseScale
               << " detailStrength=" << c->detailStrength
               << " weatherScale=" << c->weatherScale
               << " typeBias=" << c->cloudTypeBias
               << " anvil=" << c->anvilBias
               << " windDir=" << c->windDirection.x << "_" << c->windDirection.y << "_" << c->windDirection.z
               << " windSpeed=" << c->windSpeed
               << " aniso=" << c->anisotropy
               << " backG=" << c->phaseBackG
               << " phaseBlend=" << c->phaseBlend
               << " silverI=" << c->silverIntensity
               << " silverS=" << c->silverSpread
               << " extinction=" << c->extinction
               << " ambient=" << c->ambientStrength
               << " ambientTint=" << c->ambientTint.x << "_" << c->ambientTint.y << "_" << c->ambientTint.z
               << " color=" << c->cloudColor.x << "_" << c->cloudColor.y << "_" << c->cloudColor.z
               << " maxSteps=" << c->maxSteps
               << " temporal=" << (c->temporalUpsample ? 1 : 0)
               << " shadowOn=" << (c->cloudShadowEnabled ? 1 : 0)
               << " shadowStrength=" << c->cloudShadowStrength
               << " shadowExtent=" << c->cloudShadowExtent
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            // Reader defaults mirror the CloudComponent struct defaults, so a line without a key gets the
            // struct's value. They cover only absent keys: a scene that wrote extinction / ambient /
            // weatherScale / maxSteps under the earlier lighting / coverage model keeps those (now wrong)
            // values, since overriding an authored value silently would be worse. Reset those four in the
            // Inspector, or remove and re-add the component.
            CloudComponent c;
            c.enabled          = GetI(kv, "enabled", 0) != 0;
            c.bottomAltitude   = GetF(kv, "bottom",         1500.f);
            c.topAltitude      = GetF(kv, "top",            4000.f);
            c.coverage         = GetF(kv, "coverage",       0.5f);
            c.density          = GetF(kv, "density",        1.0f);
            c.noiseScale       = GetF(kv, "noiseScale",     0.00025f);
            c.detailNoiseScale = GetF(kv, "detailScale",    0.002f);
            c.detailStrength   = GetF(kv, "detailStrength", 0.30f);
            c.weatherScale     = GetF(kv, "weatherScale",   0.00006f);
            c.cloudTypeBias    = GetF(kv, "typeBias",       0.f);
            c.anvilBias        = GetF(kv, "anvil",          0.f);
            {
                auto cit = kv.find("windDir");
                if (cit != kv.end())
                    sscanf_s(cit->second.c_str(), "%f_%f_%f",
                             &c.windDirection.x, &c.windDirection.y, &c.windDirection.z);
            }
            c.windSpeed        = GetF(kv, "windSpeed",  8.f);
            c.anisotropy       = GetF(kv, "aniso",      0.6f);
            c.phaseBackG       = GetF(kv, "backG",      -0.2f);
            c.phaseBlend       = GetF(kv, "phaseBlend", 0.3f);
            c.silverIntensity  = GetF(kv, "silverI",    0.8f);
            c.silverSpread     = GetF(kv, "silverS",    0.25f);
            c.extinction       = GetF(kv, "extinction", 0.02f);
            c.ambientStrength  = GetF(kv, "ambient",    0.06f);
            {
                auto cit = kv.find("ambientTint");
                if (cit != kv.end())
                    sscanf_s(cit->second.c_str(), "%f_%f_%f",
                             &c.ambientTint.x, &c.ambientTint.y, &c.ambientTint.z);
            }
            {
                auto cit = kv.find("color");
                if (cit != kv.end())
                    sscanf_s(cit->second.c_str(), "%f_%f_%f",
                             &c.cloudColor.x, &c.cloudColor.y, &c.cloudColor.z);
            }
            c.maxSteps         = GetF(kv, "maxSteps",   128.f);
            c.temporalUpsample = GetI(kv, "temporal",   1) != 0;
            c.cloudShadowEnabled  = GetI(kv, "shadowOn", 1) != 0;
            c.cloudShadowStrength = GetF(kv, "shadowStrength", 1.0f);
            c.cloudShadowExtent   = GetF(kv, "shadowExtent",   20000.f);
            w.AddComponent<CloudComponent>(e, c);
        }
    });

    // ==== HeightFogComponent (INLINE, UE-style exponential height fog) ====
    reg.Register(std::type_index(typeid(HeightFogComponent)), {
        "HeightFog",
        [](World& w, Entity e) { return w.GetComponent<HeightFogComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* f = w.GetComponent<HeightFogComponent>(e);
            if (!f) return;
            ss << "  HeightFog: enabled=" << (f->enabled ? 1 : 0)
               << " density="    << f->fogDensity
               << " falloff="    << f->fogHeightFalloff
               << " height="     << f->fogHeight
               << " start="      << f->startDistance
               << " maxOpacity=" << f->maxOpacity
               << " color=" << f->fogColor.x << "_" << f->fogColor.y << "_" << f->fogColor.z
               << " sun="        << f->sunInscatterIntensity
               << " aniso="      << f->anisotropy;
            // The medium albedo only when it is not white: a line written before the key existed
            // reads back as written.
            if (f->albedo.x != 1.0f || f->albedo.y != 1.0f || f->albedo.z != 1.0f)
                ss << " albedo=" << f->albedo.x << "_" << f->albedo.y << "_" << f->albedo.z;
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            // Reader defaults mirror the HeightFogComponent struct defaults.
            HeightFogComponent f;
            f.enabled               = GetI(kv, "enabled", 0) != 0;
            f.fogDensity            = GetF(kv, "density",    0.002f);
            f.fogHeightFalloff      = GetF(kv, "falloff",    0.02f);
            f.fogHeight             = GetF(kv, "height",     0.f);
            f.startDistance         = GetF(kv, "start",      0.f);
            f.maxOpacity            = GetF(kv, "maxOpacity", 1.f);
            {
                auto cit = kv.find("color");
                if (cit != kv.end())
                    sscanf_s(cit->second.c_str(), "%f_%f_%f",
                             &f.fogColor.x, &f.fogColor.y, &f.fogColor.z);
            }
            f.sunInscatterIntensity = GetF(kv, "sun",        1.f);
            f.anisotropy            = GetF(kv, "aniso",      0.7f);
            {
                auto ait = kv.find("albedo");
                if (ait != kv.end())
                    sscanf_s(ait->second.c_str(), "%f_%f_%f", &f.albedo.x, &f.albedo.y, &f.albedo.z);
            }
            w.AddComponent<HeightFogComponent>(e, f);
        }
    });

    // "r_g_b" triple, the colour convention every inline serializer here uses.
    // A missing key keeps the caller's default.
    auto getF3 = [](const KVMap& kv, const char* key, DirectX::XMFLOAT3& v) {
        auto it = kv.find(key);
        if (it != kv.end())
            sscanf_s(it->second.c_str(), "%f_%f_%f", &v.x, &v.y, &v.z);
    };
    auto putF3 = [](std::ostringstream& ss, const char* key, const DirectX::XMFLOAT3& v) {
        ss << " " << key << "=" << v.x << "_" << v.y << "_" << v.z;
    };

    // ==== VolumetricFogComponent (INLINE, global froxel medium) ====
    // Presence is the ownership switch (see VolumetricFogComponent.h), so a scene saved with it stays
    // component-driven on reload. Reader defaults = struct defaults.
    reg.Register(std::type_index(typeid(VolumetricFogComponent)), {
        "VolumetricFog",
        [](World& w, Entity e) { return w.GetComponent<VolumetricFogComponent>(e) != nullptr; },
        [putF3](World& w, Entity e, std::ostringstream& ss) {
            const auto* f = w.GetComponent<VolumetricFogComponent>(e);
            if (!f) return;
            ss << "  VolumetricFog: enabled=" << (f->enabled ? 1 : 0)
               << " density="  << f->density;
            putF3(ss, "albedo", f->albedo);
            ss << " aniso="    << f->anisotropy
               << " base="     << f->baseHeight
               << " falloff="  << f->heightFalloff;
            putF3(ss, "ambient", f->ambientColor);
            ss << " ambientI=" << f->ambientIntensity;
            putF3(ss, "emissive", f->emissive);
            ss << " emissiveI=" << f->emissiveIntensity
               << " start="    << f->startDistance
               << " dist="     << f->viewDistance
               << " godRays="  << (f->godRays ? 1 : 0)
               << " temporal=" << (f->temporal ? 1 : 0)
               << " tBlend="   << f->temporalBlend
               << "\n";
        },
        [getF3](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            VolumetricFogComponent f;
            f.enabled           = GetI(kv, "enabled", f.enabled ? 1 : 0) != 0;
            f.density           = GetF(kv, "density",   f.density);
            getF3(kv, "albedo", f.albedo);
            f.anisotropy        = GetF(kv, "aniso",     f.anisotropy);
            f.baseHeight        = GetF(kv, "base",      f.baseHeight);
            f.heightFalloff     = GetF(kv, "falloff",   f.heightFalloff);
            getF3(kv, "ambient", f.ambientColor);
            f.ambientIntensity  = GetF(kv, "ambientI",  f.ambientIntensity);
            getF3(kv, "emissive", f.emissive);
            f.emissiveIntensity = GetF(kv, "emissiveI", f.emissiveIntensity);
            f.startDistance     = GetF(kv, "start",     f.startDistance);
            f.viewDistance      = GetF(kv, "dist",      f.viewDistance);
            f.godRays           = GetI(kv, "godRays",  f.godRays  ? 1 : 0) != 0;
            f.temporal          = GetI(kv, "temporal", f.temporal ? 1 : 0) != 0;
            f.temporalBlend     = GetF(kv, "tBlend",    f.temporalBlend);
            w.AddComponent<VolumetricFogComponent>(e, f);
        }
    });

    // ==== LocalFogVolumeComponent (INLINE, bounds come from the Transform) ====
    reg.Register(std::type_index(typeid(LocalFogVolumeComponent)), {
        "LocalFogVolume",
        [](World& w, Entity e) { return w.GetComponent<LocalFogVolumeComponent>(e) != nullptr; },
        [putF3](World& w, Entity e, std::ostringstream& ss) {
            const auto* v = w.GetComponent<LocalFogVolumeComponent>(e);
            if (!v) return;
            ss << "  LocalFogVolume: enabled=" << (v->enabled ? 1 : 0)
               << " shape="   << static_cast<int>(v->shape)
               << " density=" << v->density;
            putF3(ss, "albedo", v->albedo);
            putF3(ss, "emissive", v->emissive);
            ss << " emissiveI=" << v->emissiveIntensity
               << " edge="    << v->edgeFade
               << " falloff=" << v->heightFalloff
               << "\n";
        },
        [getF3](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            LocalFogVolumeComponent v;
            v.enabled           = GetI(kv, "enabled", v.enabled ? 1 : 0) != 0;
            const int shape     = GetI(kv, "shape", static_cast<int>(v.shape));
            v.shape             = shape == static_cast<int>(LocalFogShape::Sphere)
                                ? LocalFogShape::Sphere : LocalFogShape::Box;
            v.density           = GetF(kv, "density",   v.density);
            getF3(kv, "albedo", v.albedo);
            getF3(kv, "emissive", v.emissive);
            v.emissiveIntensity = GetF(kv, "emissiveI", v.emissiveIntensity);
            v.edgeFade          = GetF(kv, "edge",      v.edgeFade);
            v.heightFalloff     = GetF(kv, "falloff",   v.heightFalloff);
            w.AddComponent<LocalFogVolumeComponent>(e, v);
        }
    });

    // ==== TerrainComponent (heightmap tile + PBR layers) ====
    reg.Register(std::type_index(typeid(TerrainComponent)), {
        "Terrain",
        [](World& w, Entity e) { return w.GetComponent<TerrainComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* t = w.GetComponent<TerrainComponent>(e);
            if (!t) return;
            ss << "  Terrain: heightmap=" << PercentEncode(t->heightmapPath)
               << " center=" << t->worldCenter.x << "_" << t->worldCenter.y << "_" << t->worldCenter.z
               << " size=" << t->worldSize
               << " heightScale=" << t->heightScale
               << " tiles=" << t->tilesPerSide
               << " uvOff=" << t->heightmapUVOffset.x << "_" << t->heightmapUVOffset.y
               << " uvScale=" << t->heightmapUVScale.x << "_" << t->heightmapUVScale.y
               << " splatmap=" << PercentEncode(t->splatmapPath)
               << " layerCount=" << t->layers.size()
               << " hbOn=" << (t->heightBlendEnabled ? 1 : 0)
               << " hbStr=" << t->heightBlendStrength
               << " hbRng=" << t->heightBlendRange;
            for (size_t li = 0; li < t->layers.size(); ++li)
            {
                const auto& l = t->layers[li];
                ss << " l" << li << "a=" << PercentEncode(l.albedoPath)
                   << " l" << li << "n=" << PercentEncode(l.normalPath)
                   << " l" << li << "r=" << PercentEncode(l.armPath)
                   << " l" << li << "d=" << PercentEncode(l.dispPath)
                   << " l" << li << "t=" << l.tilingScale
                   << " l" << li << "h=" << l.minHeight << "_" << l.maxHeight << "_" << l.fadeHeight
                   << " l" << li << "s=" << l.minSlopeDeg << "_" << l.maxSlopeDeg << "_" << l.fadeSlopeDeg;
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            TerrainComponent t;
            auto getS = [&kv](const std::string& key) -> std::string {
                auto it = kv.find(key);
                return (it != kv.end()) ? PercentDecode(it->second) : std::string();
            };
            t.heightmapPath = getS("heightmap");
            t.splatmapPath  = getS("splatmap");
            {
                auto it = kv.find("center");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &t.worldCenter.x, &t.worldCenter.y, &t.worldCenter.z);
            }
            t.worldSize    = GetF(kv, "size",        1024.f);
            t.heightScale  = GetF(kv, "heightScale", 100.f);
            t.tilesPerSide = GetU(kv, "tiles",       128u);
            {
                auto it = kv.find("uvOff");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f",
                             &t.heightmapUVOffset.x, &t.heightmapUVOffset.y);
            }
            {
                auto it = kv.find("uvScale");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f",
                             &t.heightmapUVScale.x, &t.heightmapUVScale.y);
            }
            // Height-correlated blend (global). Absent -> disabled: the plain linear blend.
            t.heightBlendEnabled  = GetU(kv, "hbOn", 0u) != 0u;
            t.heightBlendStrength = GetF(kv, "hbStr", t.heightBlendStrength);
            t.heightBlendRange    = GetF(kv, "hbRng", t.heightBlendRange);

            // The layer count is data-driven; a line without layerCount has exactly 4 layers.
            uint32_t layerCount = GetU(kv, "layerCount", 4u);
            if (layerCount > 64u) layerCount = 64u;   // guard against a corrupt file
            t.layers.assign(layerCount, TerrainLayer{});
            for (uint32_t li = 0; li < layerCount; ++li)
            {
                auto& l = t.layers[li];
                const std::string p = "l" + std::to_string(li);
                l.albedoPath  = getS(p + "a");
                l.normalPath  = getS(p + "n");
                l.armPath     = getS(p + "r");
                l.dispPath    = getS(p + "d");
                l.tilingScale = GetF(kv, (p + "t").c_str(), l.tilingScale);
                auto it = kv.find(p + "h");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &l.minHeight, &l.maxHeight, &l.fadeHeight);
                it = kv.find(p + "s");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &l.minSlopeDeg, &l.maxSlopeDeg, &l.fadeSlopeDeg);
            }
            w.AddComponent<TerrainComponent>(e, std::move(t));
        }
    });

    // ==== GrassComponent (procedural GoT-style grass field) ====
    reg.Register(std::type_index(typeid(GrassComponent)), {
        "Grass",
        [](World& w, Entity e) { return w.GetComponent<GrassComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* g = w.GetComponent<GrassComponent>(e);
            if (!g) return;
            ss << "  Grass: enabled=" << (g->enabled ? 1 : 0)
               << " center=" << g->worldCenter.x << "_" << g->worldCenter.y << "_" << g->worldCenter.z
               << " size=" << g->worldSize
               << " patches=" << g->patchesPerSide
               << " density=" << g->density
               << " lod=" << g->lod0Dist << "_" << g->lod1Dist << "_" << g->cullDist
               << " blade=" << g->bladeHeight << "_" << g->bladeHeightVar << "_"
                            << g->bladeWidth << "_" << g->tiltMaxDeg << "_" << g->bendAmount
               << " windDir=" << g->windDir.x << "_" << g->windDir.y
               << " wind=" << g->windStrength << "_" << g->windSpeed << "_" << g->windScale
               << " clump=" << g->clumpCellSize << "_" << g->clumpBlend
               << " gateY=" << g->minWorldY << "_" << g->maxWorldY
               << " maxSlope=" << g->maxSlopeDeg
               << " baseColor=" << g->baseColor.x << "_" << g->baseColor.y << "_" << g->baseColor.z
               << " tipColor=" << g->tipColor.x << "_" << g->tipColor.y << "_" << g->tipColor.z
               << " cnoise=" << g->colorNoiseScale << "_" << g->colorNoiseAmount
               << " look=" << g->rootAO << "_" << g->normalBlend << "_" << g->viewThicken
                           << "_" << g->farWidthMul << "_" << g->roughness
               << " seed=" << g->seed
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            GrassComponent g;
            g.enabled = GetI(kv, "enabled", 1) != 0;
            {
                auto it = kv.find("center");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &g.worldCenter.x, &g.worldCenter.y, &g.worldCenter.z);
            }
            g.worldSize      = GetF(kv, "size",    1024.f);
            g.patchesPerSide = GetU(kv, "patches", 256u);
            g.density        = GetF(kv, "density", 8.f);
            {
                auto it = kv.find("lod");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &g.lod0Dist, &g.lod1Dist, &g.cullDist);
            }
            {
                auto it = kv.find("blade");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f_%f_%f",
                             &g.bladeHeight, &g.bladeHeightVar, &g.bladeWidth,
                             &g.tiltMaxDeg, &g.bendAmount);
            }
            {
                auto it = kv.find("windDir");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f", &g.windDir.x, &g.windDir.y);
            }
            {
                auto it = kv.find("wind");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &g.windStrength, &g.windSpeed, &g.windScale);
            }
            {
                auto it = kv.find("clump");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f",
                             &g.clumpCellSize, &g.clumpBlend);
            }
            {
                auto it = kv.find("gateY");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f", &g.minWorldY, &g.maxWorldY);
            }
            g.maxSlopeDeg = GetF(kv, "maxSlope", 38.f);
            {
                auto it = kv.find("baseColor");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &g.baseColor.x, &g.baseColor.y, &g.baseColor.z);
            }
            {
                auto it = kv.find("tipColor");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &g.tipColor.x, &g.tipColor.y, &g.tipColor.z);
            }
            {
                auto it = kv.find("cnoise");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f",
                             &g.colorNoiseScale, &g.colorNoiseAmount);
            }
            {
                auto it = kv.find("look");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f_%f_%f",
                             &g.rootAO, &g.normalBlend, &g.viewThicken,
                             &g.farWidthMul, &g.roughness);
            }
            g.seed = GetU(kv, "seed", 1337u);
            w.AddComponent<GrassComponent>(e, g);
        }
    });

    // ==== WaterInteractionComponent (ripples for a Water-domain surface) ====
    // Placement / size come from the entity's transform and are not serialized: this is the simulation
    // budget only.
    reg.Register(std::type_index(typeid(WaterInteractionComponent)), {
        "WaterRipples",
        [](World& w, Entity e) { return w.GetComponent<WaterInteractionComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<WaterInteractionComponent>(e);
            if (!c) return;
            ss << "  WaterRipples: enabled=" << (c->enabled ? 1 : 0)
               << " sim=" << c->propagation << "_" << c->damping
               << "_" << c->amplitude << "_" << c->normalStrength
               << " area=" << c->areaSize
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            WaterInteractionComponent c;
            c.enabled = GetI(kv, "enabled", 1) != 0;
            {
                auto it = kv.find("sim");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f_%f",
                             &c.propagation, &c.damping, &c.amplitude, &c.normalStrength);
            }
            c.areaSize = GetF(kv, "area", 120.0f);
            w.AddComponent<WaterInteractionComponent>(e, c);
        }
    });

    // ==== WaterComponent (flat water tile, Fresnel + flow normals) ====
    reg.Register(std::type_index(typeid(WaterComponent)), {
        "Water",
        [](World& w, Entity e) { return w.GetComponent<WaterComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<WaterComponent>(e);
            if (!c) return;
            ss << "  Water: enabled=" << (c->enabled ? 1 : 0)
               << " center=" << c->worldCenter.x << "_" << c->worldCenter.y << "_" << c->worldCenter.z
               << " size=" << c->worldSize
               << " deep=" << c->deepColor.x << "_" << c->deepColor.y << "_" << c->deepColor.z
               << " shallow=" << c->shallowColor.x << "_" << c->shallowColor.y << "_" << c->shallowColor.z
               << " absorb=" << c->absorbDist
               << " shore=" << c->shoreFade
               << " flowDir=" << c->flowDir.x << "_" << c->flowDir.y
               << " flow=" << c->flowSpeed << "_" << c->normalTiling << "_" << c->normalStrength
               << " nmapA=" << PercentEncode(c->normalMapAPath)
               << " nmapB=" << PercentEncode(c->normalMapBPath)
               << " shade=" << c->fresnelF0 << "_" << c->reflStrength << "_" << c->specPower
               << " grid=" << c->gridQuads
               << " ripples=" << (c->enableRipples ? 1 : 0)
               << "_" << c->ripplePropagation << "_" << c->rippleDamping
               << "_" << c->rippleNormalStrength << "_" << c->rippleAmplitude
               << "_" << c->rippleAreaSize
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            WaterComponent c;
            c.enabled = GetI(kv, "enabled", 1) != 0;
            {
                auto it = kv.find("center");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &c.worldCenter.x, &c.worldCenter.y, &c.worldCenter.z);
            }
            c.worldSize = GetF(kv, "size", 1024.f);
            {
                auto it = kv.find("deep");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &c.deepColor.x, &c.deepColor.y, &c.deepColor.z);
            }
            {
                auto it = kv.find("shallow");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &c.shallowColor.x, &c.shallowColor.y, &c.shallowColor.z);
            }
            c.absorbDist = GetF(kv, "absorb", 6.f);
            c.shoreFade  = GetF(kv, "shore",  1.2f);
            {
                auto it = kv.find("flowDir");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f", &c.flowDir.x, &c.flowDir.y);
            }
            {
                auto it = kv.find("flow");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &c.flowSpeed, &c.normalTiling, &c.normalStrength);
            }
            // Overwrite only when the key exists, so an older line keeps the component's default flow-map
            // paths.
            {
                auto it = kv.find("nmapA");
                if (it != kv.end()) c.normalMapAPath = PercentDecode(it->second);
            }
            {
                auto it = kv.find("nmapB");
                if (it != kv.end()) c.normalMapBPath = PercentDecode(it->second);
            }
            {
                auto it = kv.find("shade");
                if (it != kv.end())
                    sscanf_s(it->second.c_str(), "%f_%f_%f",
                             &c.fresnelF0, &c.reflStrength, &c.specPower);
            }
            c.gridQuads = GetU(kv, "grid", 64u);
            // Interactive ripples: a line without the key keeps the defaults.
            {
                auto it = kv.find("ripples");
                if (it != kv.end())
                {
                    int en = 0;
                    // rippleAreaSize is the 6th field; an older 5-field value leaves it at the component default
                    // (sscanf parses what is present).
                    sscanf_s(it->second.c_str(), "%d_%f_%f_%f_%f_%f",
                             &en, &c.ripplePropagation, &c.rippleDamping,
                             &c.rippleNormalStrength, &c.rippleAmplitude,
                             &c.rippleAreaSize);
                    c.enableRipples = en != 0;
                }
            }
            w.AddComponent<WaterComponent>(e, c);
        }
    });

    // ==== SunLightTag / MoonLightTag (tags: empty, presence only) ====
    // Serialized as a single token; the loader just adds the component.
    reg.Register(std::type_index(typeid(SunLightTag)), {
        "SunLight",
        [](World& w, Entity e) { return w.GetComponent<SunLightTag>(e) != nullptr; },
        [](World&, Entity, std::ostringstream& ss) { ss << "  SunLight: 1\n"; },
        [](World& w, Entity e, const KVMap&, Resource::AssetManager*) {
            w.AddComponent<SunLightTag>(e, SunLightTag{});
        }
    });
    reg.Register(std::type_index(typeid(MoonLightTag)), {
        "MoonLight",
        [](World& w, Entity e) { return w.GetComponent<MoonLightTag>(e) != nullptr; },
        [](World&, Entity, std::ostringstream& ss) { ss << "  MoonLight: 1\n"; },
        [](World& w, Entity e, const KVMap&, Resource::AssetManager*) {
            w.AddComponent<MoonLightTag>(e, MoonLightTag{});
        }
    });

    // ==== MaterialSourcePath (REF: the .imat path; the deserializer loads it) ====
    reg.Register(std::type_index(typeid(MaterialSourcePath)), {
        "MaterialRef",
        [](World& w, Entity e) { return w.GetComponent<MaterialSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* msp = w.GetComponent<MaterialSourcePath>(e);
            if (!msp || msp->path.empty()) return;
            ss << "  MaterialRef: path=" << PercentEncode(Sys::RelativizeAssetPath(msp->path)) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            // Material paths may arrive from .iscn joins (native '\\' on
            // Windows), prefab/world refs ('/'), or an absolute file-dialog
            // path. Collapse those aliases before comparing or storing them.
            std::string path = Sys::RelativizeAssetPath(Sys::PathToUtf8(
                Sys::U8Path(GetS(kv, "path")).lexically_normal()));
            if (path.empty()) return;

            // A skinned prefab first reconstructs its .iscn subtree, which has already loaded every F-line
            // material, then replays its component blocks so overrides can apply. Do not parse the same .imat
            // a second time: reuse it on an exact canonical source match (slash differences hide that both
            // refs name one asset). A prefab-authored replacement material (a different path) still loads,
            // and each entity keeps its own MaterialComponent copy.
            const auto* existingSource = w.GetComponent<MaterialSourcePath>(e);
            const bool alreadyLoaded =
                w.HasComponent<MaterialComponent>(e) && existingSource &&
                Sys::RelativizeAssetPath(Sys::PathToUtf8(
                    Sys::U8Path(existingSource->path).lexically_normal())) == path;

            w.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ path });
            if (alreadyLoaded)
                return;

            // Load the .imat into MaterialComponent
            MaterialComponent mat{};
            Resource::MaterialLoadInfo info;
            if (Resource::LoadMaterial(path, mat, &info))
            {
                mat.SetDirty();
                if (auto* existing = w.GetComponent<MaterialComponent>(e))
                    *existing = mat;
                else
                    w.AddComponent<MaterialComponent>(e, std::move(mat));
                // Provenance rides along, or the entity forgets which of its
                // values are its own the moment the file finishes loading.
                ApplyMaterialLoadInfo(w, e, info);
            }
        }
    });

    // ==== MaterialComponent (INLINE, entities without a MaterialRef) ====
    // Ref-based entities persist via MaterialRef + overrides. An entity whose material has no .imat
    // source (a trail / particle whose look fields were migrated into a synthesized material, a
    // hand-added Material on a VFX entity) would otherwise lose its material on save / reload. Writes
    // the full .imat key=value text (one format source: MaterialSerializer::BuildMaterialText),
    // strict-escaped into one KV value. The two never coexist on save (has() excludes ref'd
    // entities), so the load order against MaterialRef does not matter.
    reg.Register(std::type_index(typeid(MaterialComponent)), {
        "MaterialInline",
        [](World& w, Entity e) {
            return w.GetComponent<MaterialComponent>(e) != nullptr &&
                   w.GetComponent<MaterialSourcePath>(e) == nullptr;
        },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* mc = w.GetComponent<MaterialComponent>(e);
            if (!mc) return;
            if (w.GetComponent<MaterialSourcePath>(e)) return;   // ref path owns it
            ss << "  MaterialInline: data="
               << PercentEncodeStrict(Resource::BuildMaterialText(*mc)) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            // Decode once: GetS already percent-decodes. A second PercentDecode would mangle any material text
            // with a literal '%' (a path, a comment) and make the live entity disagree with the asset
            // preview's reader (ComposePrefabAsset -> matInlineText) about the same block.
            const std::string text = GetS(kv, "data");
            if (text.empty()) return;
            MaterialComponent mat{};
            Resource::ApplyMaterialTextInPlace(mat, text);
            if (auto* existing = w.GetComponent<MaterialComponent>(e))
                *existing = std::move(mat);
            else
                w.AddComponent<MaterialComponent>(e, std::move(mat));
        }
    });

    // ==== MeshSourcePath (REF) ====
    // A display hint only: the deserializer attaches no render handle. Visible geometry needs a
    // re-spawn via MeshLibrary (.meshlib + meshId).
    reg.Register(std::type_index(typeid(MeshSourcePath)), {
        "MeshRef",
        [](World& w, Entity e) { return w.GetComponent<MeshSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* msp = w.GetComponent<MeshSourcePath>(e);
            if (!msp || msp->path.empty()) return;
            ss << "  MeshRef: path=" << PercentEncode(msp->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager* /*assetMgr*/) {
            std::string path = GetS(kv, "path");
            if (path.empty()) return;
            w.AddComponent<MeshSourcePath>(e, MeshSourcePath{ path });
        }
    });

    // ==== StaticMeshComponent (one mesh of a model) ====
    //   StaticMesh: model=<.iscn> mesh=<MeshKey hex16> name=<mesh name> collision=asset|none
    //               ovr=<n> ovr0k=<SectionKey hex16> ovr0=<.imat> ...
    // `model` and each `ovr<i>` are plain asset paths, so the scene writer stamps a $g_ GUID beside
    // each and a moved model or material is repaired on load. The derived section entities are never
    // written (SceneTransientTag); MeshInstanceSystem rebuilds them from this line.
    reg.Register(std::type_index(typeid(StaticMeshComponent)), {
        "StaticMesh",
        [](World& w, Entity e) { return w.GetComponent<StaticMeshComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* sm = w.GetComponent<StaticMeshComponent>(e);
            if (!sm) return;
            ss << "  StaticMesh: model=" << PercentEncode(sm->model)
               << " mesh=" << Resource::MeshKeyToHex(sm->meshKey)
               << " name=" << PercentEncodeText(sm->meshName)
               << " collision=" << (sm->collision == StaticMeshComponent::Collision::None ? "none" : "asset")
               << " ovr=" << sm->slotOverrides.size();
            for (size_t i = 0; i < sm->slotOverrides.size(); ++i)
                ss << " ovr" << i << "k=" << Resource::MeshKeyToHex(sm->slotOverrides[i].sectionKey)
                   << " ovr" << i << "=" << PercentEncode(sm->slotOverrides[i].material);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            StaticMeshComponent sm;
            sm.model    = GetS(kv, "model");
            sm.meshName = GetS(kv, "name");
            if (!Resource::MeshKeyFromHex(GetS(kv, "mesh"), sm.meshKey)) sm.meshKey = 0;
            sm.collision = (GetS(kv, "collision") == "none") ? StaticMeshComponent::Collision::None
                                                            : StaticMeshComponent::Collision::Asset;
            const uint32_t n = (std::min)(GetU(kv, "ovr", 0), 1024u);
            for (uint32_t i = 0; i < n; ++i)
            {
                const std::string idx = std::to_string(i);
                StaticMeshComponent::SlotOverride o;
                o.material = GetS(kv, ("ovr" + idx).c_str());
                if (Resource::MeshKeyFromHex(GetS(kv, ("ovr" + idx + "k").c_str()), o.sectionKey) &&
                    o.sectionKey != 0 && !o.material.empty())
                    sm.slotOverrides.push_back(std::move(o));
            }
            if (auto* existing = w.GetComponent<StaticMeshComponent>(e))
                *existing = std::move(sm);
            else
                w.AddComponent<StaticMeshComponent>(e, std::move(sm));
        }
    });

    // ==== SceneSourcePath (REF, for skinned characters) ====
    reg.Register(std::type_index(typeid(SceneSourcePath)), {
        "SceneRef",
        [](World& w, Entity e) { return w.GetComponent<SceneSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ssp = w.GetComponent<SceneSourcePath>(e);
            if (!ssp || ssp->path.empty()) return;
            ss << "  SceneRef: path=" << PercentEncode(ssp->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            std::string path = GetS(kv, "path");
            if (!path.empty())
                w.AddComponent<SceneSourcePath>(e, SceneSourcePath{ path });
        }
    });

    // ==== PrefabSourcePath (REF, prefab instance root) ====
    // Written into scene files only (SaveScene writes no other component on a prefab root: a pure
    // reference). LoadScene special-cases "PrefabRef" rows and re-instantiates the .ipfb; this
    // deserialize is the fallback that keeps the tag round-trippable. LoadPrefab's own registry walk
    // ignores tags prefixed "Prefab", so a stale self-ref line inside a .ipfb cannot recurse.
    reg.Register(std::type_index(typeid(PrefabSourcePath)), {
        "PrefabRef",
        [](World& w, Entity e) { return w.GetComponent<PrefabSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* psp = w.GetComponent<PrefabSourcePath>(e);
            if (!psp || psp->path.empty()) return;
            ss << "  PrefabRef: path=" << PercentEncode(psp->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            std::string path = GetS(kv, "path");
            if (!path.empty() && !w.HasComponent<PrefabSourcePath>(e))
                w.AddComponent<PrefabSourcePath>(e, PrefabSourcePath{ path });
        }
    });

    // ==== AnimationSourcePath (REF) ====
    reg.Register(std::type_index(typeid(AnimationSourcePath)), {
        "AnimRef",
        [](World& w, Entity e) { return w.GetComponent<AnimationSourcePath>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* asp = w.GetComponent<AnimationSourcePath>(e);
            if (!asp || asp->path.empty()) return;
            ss << "  AnimRef: path=" << PercentEncode(asp->path) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            std::string path = GetS(kv, "path");
            if (!path.empty())
                w.AddComponent<AnimationSourcePath>(e, AnimationSourcePath{ path });
        }
    });

    reg.Register(std::type_index(typeid(AnimGraphComponent)), {
        "AnimGraph",
        [](World& w, Entity e) { return w.HasComponent<AnimGraphComponent>(e); },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* graph = w.GetComponent<AnimGraphComponent>(e); if (!graph) return;
            ss << "  AnimGraph: path=" << PercentEncode(graph->graphPath)
               << " set=" << PercentEncode(graph->animSetPath) << " enabled=" << graph->enabled << " motorRoot=" << graph->motorRootMotion << " lod=" << graph->lod << " interval=" << graph->updateInterval;
            nlohmann::json defaults = nlohmann::json::array();
            for (const auto& pair : graph->defaults) defaults.push_back({{"name", pair.first}, {"v", pair.second.vector}, {"i", pair.second.integer}});
            ss << " defaults=" << PercentEncode(defaults.dump()) << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AnimGraphComponent graph; graph.graphPath = GetS(kv, "path", "");
            graph.animSetPath = GetS(kv, "set", ""); graph.enabled = GetI(kv, "enabled", 1) != 0;graph.motorRootMotion=GetI(kv,"motorRoot",0)!=0;graph.lod=std::max(0,GetI(kv,"lod",0));graph.updateInterval=std::clamp(GetI(kv,"interval",1),1,60);
            try {
                const auto defaults = nlohmann::json::parse(GetS(kv, "defaults", "[]"));
                for (const auto& j : defaults) { Anim::Graph::ParamValue value; value.vector = j.at("v").get<std::array<float, 3>>(); value.integer = j.at("i").get<uint32_t>(); graph.defaults.emplace_back(j.at("name").get<std::string>(), value); }
            } catch (const std::exception&) { LOG_WARNING("AnimGraph: invalid default parameter overrides"); }
            w.AddComponent<AnimGraphComponent>(e, std::move(graph));
        }
    });

    // ==== AnimationSetComponent (AUTHORED) ====
    // Stable aliases + paths only. Runtime handles, ClipLibrary indices and
    // transition state are owned by AnimationClipSystem/Animator components.
    reg.Register(std::type_index(typeid(AnimationSetComponent)), {
        "AnimationSet",
        [](World& w, Entity e) { return w.GetComponent<AnimationSetComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* set = w.GetComponent<AnimationSetComponent>(e);
            if (!set) return;
            ss << "  AnimationSet: default=" << PercentEncode(set->defaultClip)
               << " autoplay=" << (set->playDefaultOnLoad ? 1 : 0)
               << " n=" << set->clips.size();
            for (size_t i = 0; i < set->clips.size(); ++i)
            {
                const AnimationClipReference& clip = set->clips[i];
                ss << " name" << i << '=' << PercentEncode(clip.name)
                   << " path" << i << '=' << PercentEncode(clip.path)
                   << " bone" << i << '=' << PercentEncode(clip.sourceClip)
                   << " morph" << i << '=' << PercentEncode(clip.sourceMorphClip)
                   << " loop" << i << '=' << (clip.looping ? 1 : 0)
                   << " speed" << i << '=' << clip.speed;
            }
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AnimationSetComponent set;
            set.defaultClip       = GetS(kv, "default", "");
            set.playDefaultOnLoad = GetI(kv, "autoplay", 0) != 0;
            const int count = std::clamp(GetI(kv, "n", 0), 0, 128);
            set.clips.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                char key[24];
                AnimationClipReference clip;
                snprintf(key, sizeof(key), "name%d", i);  clip.name    = GetS(kv, key, "");
                snprintf(key, sizeof(key), "path%d", i);  clip.path    = GetS(kv, key, "");
                snprintf(key, sizeof(key), "bone%d", i);  clip.sourceClip = GetS(kv, key, "");
                snprintf(key, sizeof(key), "morph%d", i); clip.sourceMorphClip = GetS(kv, key, "");
                snprintf(key, sizeof(key), "loop%d", i);  clip.looping = GetI(kv, key, 1) != 0;
                snprintf(key, sizeof(key), "speed%d", i); clip.speed   = GetF(kv, key, 1.f);
                if (!clip.name.empty() && !clip.path.empty())
                    set.clips.push_back(std::move(clip));
            }
            w.AddComponent<AnimationSetComponent>(e, std::move(set));
        }
    });

    // ==== CharacterStateComponent (COMPATIBILITY LUA FSM DEFINITIONS) ====
    // Persist authored definitions/default blend only. Current/pending indices
    // and Resource handles intentionally restart as runtime state.
    reg.Register(std::type_index(typeid(CharacterStateComponent)), {
        "CharacterStates",
        [](World& w, Entity e) { return w.GetComponent<CharacterStateComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* states = w.GetComponent<CharacterStateComponent>(e);
            if (!states) return;
            ss << "  CharacterStates: defaultBlend=" << states->defaultBlendDuration
               << " n=" << states->states.size();
            for (size_t i = 0; i < states->states.size(); ++i)
            {
                const auto& state = states->states[i];
                ss << " name" << i << '=' << PercentEncode(state.name)
                   << " path" << i << '=' << PercentEncode(state.clipPath)
                   << " loop" << i << '=' << (state.looping ? 1 : 0)
                   << " speed" << i << '=' << state.playbackSpeed;
            }
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CharacterStateComponent states;
            states.defaultBlendDuration = GetF(kv, "defaultBlend", 0.2f);
            const int count = std::clamp(GetI(kv, "n", 0), 0, 128);
            states.states.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                char key[24];
                CharacterStateComponent::StateEntry state;
                snprintf(key, sizeof(key), "name%d", i);  state.name          = GetS(kv, key, "");
                snprintf(key, sizeof(key), "path%d", i);  state.clipPath      = GetS(kv, key, "");
                snprintf(key, sizeof(key), "loop%d", i);  state.looping       = GetI(kv, key, 1) != 0;
                snprintf(key, sizeof(key), "speed%d", i); state.playbackSpeed = GetF(kv, key, 1.f);
                if (!state.name.empty() && !state.clipPath.empty())
                    states.states.push_back(std::move(state));
            }
            w.AddComponent<CharacterStateComponent>(e, std::move(states));
        }
    });

    // ==== MaterialOverride (INLINE, property bag diff) ====
    reg.Register(std::type_index(typeid(MaterialOverride)), {
        "MaterialOverride",
        [](World& w, Entity e) { return w.GetComponent<MaterialOverride>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ovr = w.GetComponent<MaterialOverride>(e);
            if (!ovr || ovr->IsEmpty()) return;
            ss << "  MaterialOverride:";
            for (const auto& [key, val] : ovr->props)
            {
                ss << " " << PercentEncode(key) << "=";
                switch (val.type)
                {
                case MaterialOverride::Value::Float: ss << "f:" << val.data.f; break;
                case MaterialOverride::Value::Vec4:
                    ss << "v:" << val.data.v4[0] << "_" << val.data.v4[1]
                       << "_" << val.data.v4[2] << "_" << val.data.v4[3]; break;
                case MaterialOverride::Value::Int:   ss << "i:" << val.data.i; break;
                case MaterialOverride::Value::Bool:  ss << "b:" << (val.data.b ? 1 : 0); break;
                case MaterialOverride::Value::TexRef:ss << "t:" << PercentEncode(val.texPath); break;
                }
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            // The parse lives in Resource::ParseMaterialOverrideKV, shared with the asset preview, which reads
            // a .ipfb as data (no World, no entity): one deserializer for the on-disk grammar.
            MaterialOverride ovr;
            Resource::ParseMaterialOverrideKV(kv, ovr);
            if (!ovr.IsEmpty())
                w.AddComponent<MaterialOverride>(e, std::move(ovr));
        }
    });

    // ==== MeshHandle (INLINE, procedural primitives: 0=Cube, 1=Sphere, 2=Cone) ====
    reg.Register(std::type_index(typeid(MeshHandle)), {
        "MeshHandle",
        [](World& w, Entity e) { return w.GetComponent<MeshHandle>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* mh = w.GetComponent<MeshHandle>(e);
            if (!mh || !mh->IsValid()) return;
            char buf[64];
            snprintf(buf, sizeof(buf), "  MeshHandle: gpuMeshID=%u\n", mh->gpuMeshID);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            MeshHandle mh;
            mh.gpuMeshID = static_cast<uint32_t>(GetI(kv, "gpuMeshID", -1));
            if (mh.IsValid())
                w.AddComponent<MeshHandle>(e, mh);
        }
    });

    // ==== VisibilityComponent (INLINE) ====
    // Persists viewMask + flags + renderLayer. inheritedHidden is recomputed every frame by
    // TransformSystem from the parent chain, so it is not saved (it would round-trip a stale value).
    reg.Register(std::type_index(typeid(VisibilityComponent)), {
        "Visibility",
        [](World& w, Entity e) { return w.GetComponent<VisibilityComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* v = w.GetComponent<VisibilityComponent>(e);
            if (!v) return;
            char buf[96];
            snprintf(buf, sizeof(buf),
                "  Visibility: viewMask=%u flags=%u renderLayer=%u\n",
                v->viewMask,
                static_cast<uint32_t>(v->flags),
                static_cast<uint32_t>(v->renderLayer));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            VisibilityComponent v;
            v.viewMask    = GetU(kv, "viewMask", ViewBit::All);
            v.flags       = static_cast<uint8_t>(GetU(kv, "flags",
                                                       VisibilityComponent::Visible
                                                     | VisibilityComponent::CastShadow
                                                     | VisibilityComponent::RenderInMainPass));
            v.renderLayer = static_cast<uint8_t>(GetU(kv, "renderLayer", 0));
            w.AddComponent<VisibilityComponent>(e, v);
        }
    });

    // ==== BoundingVolume (INLINE, for mouse picking on primitives) ====
    reg.Register(std::type_index(typeid(BoundingVolume)), {
        "BoundingVolume",
        [](World& w, Entity e) { return w.GetComponent<BoundingVolume>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            ss << "  BoundingVolume:\n";
        },
        [](World& w, Entity e, const KVMap&, Resource::AssetManager*) {
            w.AddComponent<BoundingVolume>(e, BoundingVolume{});
        }
    });

    // ==== ChainPhysicsComponent (INLINE) ====
    // The authored fields below are mirrored by the CHAIN_PHYSICS_*_FIELDS X-macros (CopyAuthoredInto
    // / CopyMaterialInto / AuthoredEquals / MaterialEquals): keep the two in lockstep.
    // tests/unit/chain_preview_prefab_test P7_FieldListLockstep fails when a key here is missing from
    // them, or the reverse.
    reg.Register(std::type_index(typeid(ChainPhysicsComponent)), {
        "ChainPhysics",
        [](World& w, Entity e) { return w.GetComponent<ChainPhysicsComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* cp = w.GetComponent<ChainPhysicsComponent>(e);
            if (!cp) return;
            // Sized well past the ~1000-char worst case: an overflowing snprintf silently truncates the spring
            // tail. Wire keys are frozen: "stiffness" / "skirtStiffness" feed structuralStiffness /
            // ringStiffness (constraint-bucket materials, not a per-type preset pair); renaming the keys would
            // orphan every authored .iscn for no benefit.
            char buf[1792];
            snprintf(buf, sizeof(buf),
                "  ChainPhysics: enabled=%d damping=%.4f gravity=%.4f stiffness=%.4f"
                " iterations=%d substeps=%d maxVelocity=%.4f localStiffness=%.4f"
                " tipHold=%.4f collisionPadding=%.4f"
                " contactSkin=%.4f contactFriction=%.4f"
                " limitAngle=%.4f limitTipScale=%.4f inertiaScale=%.4f"
                " skirtDamping=%.4f skirtGravity=%.4f skirtStiffness=%.4f"
                " shearStiffness=%.4f"
                " skirtMaxVelocity=%.4f skirtLocalStiffness=%.4f skirtHorizDecay=%.4f"
                " skirtTipHold=%.4f"
                " skirtLimitAngle=%.4f skirtLimitTipScale=%.4f skirtInertiaScale=%.4f"
                " windDirX=%.4f windDirY=%.4f windDirZ=%.4f"
                " windStrength=%.4f windFreq=%.4f windTurbulence=%.4f"
                " springStiffness=%.4f springDamping=%.4f springMass=%.4f"
                " springGravity=%.4f springMaxDisp=%.4f"
                " springChildStiffness=%.4f springChildDamping=%.4f"
                " springChildMass=%.4f springChildGravity=%.4f springChildMaxDisp=%.4f",
                cp->enabled ? 1 : 0,
                cp->damping, cp->gravity, cp->structuralStiffness,
                cp->iterations, cp->substeps, cp->maxVelocity, cp->localStiffness,
                cp->tipHold, cp->collisionPadding,
                cp->contactSkin, cp->contactFriction,
                cp->limitAngle, cp->limitTipScale, cp->inertiaScale,
                cp->skirtDamping, cp->skirtGravity, cp->ringStiffness,
                cp->shearStiffness,
                cp->skirtMaxVelocity, cp->skirtLocalStiffness, cp->skirtHorizDecay,
                cp->skirtTipHold,
                cp->skirtLimitAngle, cp->skirtLimitTipScale, cp->skirtInertiaScale,
                cp->windDir.x, cp->windDir.y, cp->windDir.z,
                cp->windStrength, cp->windFreq, cp->windTurbulence,
                cp->springStiffness, cp->springDamping, cp->springMass,
                cp->springGravity, cp->springMaxDisp,
                cp->springChildStiffness, cp->springChildDamping,
                cp->springChildMass, cp->springChildGravity, cp->springChildMaxDisp);
            ss << buf;

            // Keys added after the wire keys froze follow one rule: new keys (never a rename), outside `buf`,
            // written only when the value differs from the struct default, so a component at defaults
            // re-saves byte-identically and an older reader ignores them (an unknown key is an unused KVMap
            // entry). Absent on read = the default (worldInertia 0 = no world-motion inertia). Same "%.4f" as
            // every other global on the line.
            {
                static const ChainPhysicsComponent kDef{};
                char kvb[48];
                auto put = [&](const char* k, float v, float def)
                {
                    if (v == def) return;
                    snprintf(kvb, sizeof(kvb), " %s=%.4f", k, v);
                    ss << kvb;
                };
                put("worldInertiaLoc", cp->worldInertiaLocation, kDef.worldInertiaLocation);
                put("worldInertiaRot", cp->worldInertiaRotation, kDef.worldInertiaRotation);
                put("teleportDist",    cp->teleportDistance,     kDef.teleportDistance);
                put("teleportAngle",   cp->teleportAngle,        kDef.teleportAngle);
            }

            // Authored chain groups (KawaiiPhysics-style), streamed onto the same line so the reader sees them
            // in this component's flat KVMap. Emitted only when present, so a component without groups
            // serializes byte-identically.
            if (!cp->groups.empty())
            {
                ss << " groups=" << cp->groups.size();
                for (int i = 0; i < static_cast<int>(cp->groups.size()); ++i)
                {
                    const auto& g = cp->groups[i];
                    ss << " g" << i << "_name=" << PercentEncode(g.name)
                       << " g" << i << "_root=" << PercentEncode(g.rootBone)
                       << " g" << i << "_type=" << static_cast<int>(g.type)
                       << " g" << i << "_en="   << (g.enabled ? 1 : 0)
                       << " g" << i << "_rg="   << g.ringGroup
                       << " g" << i << "_ri="   << g.ringIndex
                       << " g" << i << "_pg="   << g.pairGroupId
                       << " g" << i << "_es="    << (g.excludeSubtree ? 1 : 0)
                       << " g" << i << "_ovrEn=" << (g.ovrEnabled ? 1 : 0)
                       << " g" << i << "_ovrD="  << g.ovrDamping
                       << " g" << i << "_ovrG="  << g.ovrGravity
                       << " g" << i << "_ovrS="  << g.ovrStructuralStiffness
                       << " g" << i << "_ovrRS=" << g.ovrRingStiffness
                       << " g" << i << "_ovrShS="<< g.ovrShearStiffness
                       << " g" << i << "_ovrMV=" << g.ovrMaxVelocity
                       << " g" << i << "_ovrL="  << g.ovrLocalStiffness
                       << " g" << i << "_ovrHD=" << g.ovrHorizDecay
                       << " g" << i << "_ovrTH=" << g.ovrTipHold
                       << " g" << i << "_ovrCP=" << g.ovrCollisionPadding
                       << " g" << i << "_ovrCF=" << g.ovrContactFriction
                       << " g" << i << "_ovrLA=" << g.ovrLimitAngle
                       << " g" << i << "_ovrLT=" << g.ovrLimitTipScale
                       << " g" << i << "_ovrIN=" << g.ovrInertiaScale
                       << " g" << i << "_ovrWS=" << g.ovrWindStrength
                       << " g" << i << "_ovrWF=" << g.ovrWindFreq
                       << " g" << i << "_col="   << (g.collide ? 1 : 0)
                       << " g" << i << "_ovrSk=" << g.ovrSpringStiffness
                       << " g" << i << "_ovrSd=" << g.ovrSpringDamping
                       << " g" << i << "_ovrSm=" << g.ovrSpringMass
                       << " g" << i << "_ovrSg=" << g.ovrSpringGravity
                       << " g" << i << "_ovrSMD=" << g.ovrSpringMaxDisp
                       << " g" << i << "_ovrSCk=" << g.ovrSpringChildStiffness
                       << " g" << i << "_ovrSCd=" << g.ovrSpringChildDamping
                       << " g" << i << "_ovrSCm=" << g.ovrSpringChildMass
                       << " g" << i << "_ovrSCg=" << g.ovrSpringChildGravity
                       << " g" << i << "_ovrSCMD=" << g.ovrSpringChildMaxDisp;
                    // A group's own world-motion inertia is written only when it has one (>= 0); < 0 = follow the
                    // component, which is also what an absent key reads as, so a following group costs no bytes.
                    if (g.ovrWorldInertiaLocation >= 0.f)
                        ss << " g" << i << "_ovrWIL=" << g.ovrWorldInertiaLocation;
                    if (g.ovrWorldInertiaRotation >= 0.f)
                        ss << " g" << i << "_ovrWIR=" << g.ovrWorldInertiaRotation;
                    ss << " g" << i << "_exc="   << g.excludeCount;
                    for (int j = 0; j < g.excludeCount && j < ChainGroupDef::MAX_EXCLUDE; ++j)
                        ss << " g" << i << "_e" << j << "=" << PercentEncode(g.excludeBones[j]);
                }
            }

            // Authored rest pose, streamed like the groups: it is variable-length and would overflow the fixed
            // `buf`. rpEn / rpW are written whenever non-default, not gated on a non-empty restPose, or
            // "disabled" and a partial weight would be inexpressible while the list is empty. Both at default
            // = no extra bytes.
            if (!cp->restPoseEnabled)     ss << " rpEn=0";
            if (cp->restPoseWeight != 1.f) ss << " rpW=" << cp->restPoseWeight;
            if (!cp->restPose.empty())
            {
                // %.7g, not the stream's 6 significant digits: a quaternion round-tripped at 6 digits visibly
                // drifts on a long chain. PercentEncodeStrict because a bone name may contain '=', and
                // ParseComponentLine splits on the first '='.
                char qb[96];
                ss << " rp=" << cp->restPose.size();
                for (int i = 0; i < static_cast<int>(cp->restPose.size()); ++i)
                {
                    const auto& r = cp->restPose[i];
                    snprintf(qb, sizeof(qb), "%.7g_%.7g_%.7g_%.7g",
                             r.rot.x, r.rot.y, r.rot.z, r.rot.w);
                    ss << " r" << i << "_b=" << PercentEncodeStrict(r.boneName)
                       << " r" << i << "_q=" << qb
                       << " r" << i << "_w=" << r.weight
                       << " r" << i << "_e=" << (r.enabled ? 1 : 0);
                }
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ChainPhysicsComponent cp;
            // Absent `enabled` -> 1 (a line without the key is an enabled chain).
            cp.enabled             = GetI(kv, "enabled", 1) != 0;
            cp.damping             = GetF(kv, "damping",             cp.damping);
            cp.gravity             = GetF(kv, "gravity",             cp.gravity);
            // Wire key "stiffness" feeds structuralStiffness (see the writer).
            cp.structuralStiffness = GetF(kv, "stiffness",          cp.structuralStiffness);
            cp.iterations          = GetI(kv, "iterations",          cp.iterations);
            cp.substeps            = GetI(kv, "substeps",            cp.substeps);
            cp.maxVelocity         = GetF(kv, "maxVelocity",        cp.maxVelocity);
            cp.localStiffness      = GetF(kv, "localStiffness",     cp.localStiffness);
            cp.tipHold             = GetF(kv, "tipHold",            cp.tipHold);
            cp.skirtTipHold        = GetF(kv, "skirtTipHold",       cp.skirtTipHold);
            cp.collisionPadding    = GetF(kv, "collisionPadding",   cp.collisionPadding);
            // The struct defaults reproduce the collision solver's fixed values for a line without these keys.
            cp.contactSkin         = GetF(kv, "contactSkin",        cp.contactSkin);
            cp.contactFriction     = GetF(kv, "contactFriction",    cp.contactFriction);
            // Rest limit / inertia / sway: absent keys keep the struct defaults (limit 0 = unlimited, inertia
            // 1 = full reaction, windStrength 0 = off), which add none of these effects.
            cp.limitAngle          = GetF(kv, "limitAngle",         cp.limitAngle);
            cp.limitTipScale       = GetF(kv, "limitTipScale",      cp.limitTipScale);
            cp.inertiaScale        = GetF(kv, "inertiaScale",       cp.inertiaScale);
            cp.skirtLimitAngle     = GetF(kv, "skirtLimitAngle",    cp.skirtLimitAngle);
            cp.skirtLimitTipScale  = GetF(kv, "skirtLimitTipScale", cp.skirtLimitTipScale);
            cp.skirtInertiaScale   = GetF(kv, "skirtInertiaScale",  cp.skirtInertiaScale);
            cp.windDir.x           = GetF(kv, "windDirX",           cp.windDir.x);
            cp.windDir.y           = GetF(kv, "windDirY",           cp.windDir.y);
            cp.windDir.z           = GetF(kv, "windDirZ",           cp.windDir.z);
            cp.windStrength        = GetF(kv, "windStrength",       cp.windStrength);
            cp.windFreq            = GetF(kv, "windFreq",           cp.windFreq);
            cp.windTurbulence      = GetF(kv, "windTurbulence",     cp.windTurbulence);
            cp.skirtDamping        = GetF(kv, "skirtDamping",       cp.skirtDamping);
            cp.skirtGravity        = GetF(kv, "skirtGravity",       cp.skirtGravity);
            // Wire key "skirtStiffness" feeds ringStiffness (see the writer).
            cp.ringStiffness       = GetF(kv, "skirtStiffness",     cp.ringStiffness);
            // Absent shearStiffness -> the resolved ring value (a line without the key welds shear to ring).
            cp.shearStiffness      = GetF(kv, "shearStiffness",     cp.ringStiffness);
            cp.skirtMaxVelocity    = GetF(kv, "skirtMaxVelocity",   cp.skirtMaxVelocity);
            cp.skirtLocalStiffness = GetF(kv, "skirtLocalStiffness",cp.skirtLocalStiffness);
            cp.skirtHorizDecay     = GetF(kv, "skirtHorizDecay",    cp.skirtHorizDecay);
            cp.springStiffness     = GetF(kv, "springStiffness",    cp.springStiffness);
            cp.springDamping       = GetF(kv, "springDamping",      cp.springDamping);
            cp.springMass          = GetF(kv, "springMass",          cp.springMass);
            cp.springGravity       = GetF(kv, "springGravity",      cp.springGravity);
            cp.springMaxDisp       = GetF(kv, "springMaxDisp",      cp.springMaxDisp);
            cp.springChildStiffness = GetF(kv, "springChildStiffness", cp.springChildStiffness);
            cp.springChildDamping   = GetF(kv, "springChildDamping",   cp.springChildDamping);
            cp.springChildMass      = GetF(kv, "springChildMass",      cp.springChildMass);
            cp.springChildGravity   = GetF(kv, "springChildGravity",   cp.springChildGravity);
            cp.springChildMaxDisp   = GetF(kv, "springChildMaxDisp",   cp.springChildMaxDisp);
            // World-motion inertia: absent keys keep the struct defaults (inertia 0 = off).
            cp.worldInertiaLocation = GetF(kv, "worldInertiaLoc", cp.worldInertiaLocation);
            cp.worldInertiaRotation = GetF(kv, "worldInertiaRot", cp.worldInertiaRotation);
            cp.teleportDistance     = GetF(kv, "teleportDist",    cp.teleportDistance);
            cp.teleportAngle        = GetF(kv, "teleportAngle",   cp.teleportAngle);

            // Authored chain groups: absent -> groupCount 0 -> the runtime uses the bone-name keyword path.
            int gc = GetI(kv, "groups", 0);
            if (gc < 0) gc = 0;
            if (gc > ChainPhysicsComponent::MAX_GROUPS) gc = ChainPhysicsComponent::MAX_GROUPS;
            cp.groups.resize(static_cast<size_t>(gc));
            for (int i = 0; i < gc; ++i)
            {
                ChainGroupDef& g = cp.groups[i];
                char key[32];
                snprintf(key, sizeof(key), "g%d_name", i);
                snprintf(g.name, sizeof(g.name), "%s", GetS(kv, key).c_str());
                snprintf(key, sizeof(key), "g%d_root", i);
                snprintf(g.rootBone, sizeof(g.rootBone), "%s", GetS(kv, key).c_str());
                snprintf(key, sizeof(key), "g%d_type", i);
                g.type = static_cast<ChainGroupType>(GetI(kv, key, 0));
                // Fields newer than the per-chain overrides: seed them from the effective globals so an older
                // prefab without the keys keeps its runtime behaviour byte for byte.
                g.ovrMaxVelocity = (g.type == ChainGroupType::Cloth)
                    ? cp.skirtMaxVelocity : cp.maxVelocity;
                g.ovrHorizDecay = cp.skirtHorizDecay;
                // Seeded from the component, not the struct default (0.02, while the global's is 0.0): a group
                // saved without either padding key ran at the component's value, and must not reload with 2 cm of
                // padding it was never authored with.
                g.ovrCollisionPadding = cp.collisionPadding;
                // An overriding group written without this key reads the component's friction.
                g.ovrContactFriction  = cp.contactFriction;
                // An overriding group written without the spring max-displacement keys reads the component
                // values.
                g.ovrSpringMaxDisp      = cp.springMaxDisp;
                g.ovrSpringChildMaxDisp = cp.springChildMaxDisp;
                snprintf(key, sizeof(key), "g%d_en", i);   g.enabled        = GetI(kv, key, 1) != 0;
                snprintf(key, sizeof(key), "g%d_rg", i);   g.ringGroup      = GetI(kv, key, 0);
                snprintf(key, sizeof(key), "g%d_ri", i);   g.ringIndex      = GetI(kv, key, -1);
                snprintf(key, sizeof(key), "g%d_pg", i);   g.pairGroupId    = GetI(kv, key, -1);
                snprintf(key, sizeof(key), "g%d_es", i);    g.excludeSubtree     = GetI(kv, key, 1) != 0;
                snprintf(key, sizeof(key), "g%d_ovrEn", i); g.ovrEnabled         = GetI(kv, key, 0) != 0;
                snprintf(key, sizeof(key), "g%d_ovrD", i);  g.ovrDamping         = GetF(kv, key, g.ovrDamping);
                snprintf(key, sizeof(key), "g%d_ovrG", i);  g.ovrGravity         = GetF(kv, key, g.ovrGravity);
                // ovrS -> ovrRS -> ovrShS: each defaults to the one before it, so a line carrying only ovrS
                // reloads with that value in all three buckets.
                snprintf(key, sizeof(key), "g%d_ovrS", i);   g.ovrStructuralStiffness = GetF(kv, key, g.ovrStructuralStiffness);
                snprintf(key, sizeof(key), "g%d_ovrRS", i);  g.ovrRingStiffness       = GetF(kv, key, g.ovrStructuralStiffness);
                snprintf(key, sizeof(key), "g%d_ovrShS", i); g.ovrShearStiffness      = GetF(kv, key, g.ovrRingStiffness);
                snprintf(key, sizeof(key), "g%d_ovrMV", i); g.ovrMaxVelocity     = GetF(kv, key, g.ovrMaxVelocity);
                snprintf(key, sizeof(key), "g%d_ovrL", i);  g.ovrLocalStiffness  = GetF(kv, key, g.ovrLocalStiffness);
                snprintf(key, sizeof(key), "g%d_ovrHD", i); g.ovrHorizDecay      = GetF(kv, key, g.ovrHorizDecay);
                snprintf(key, sizeof(key), "g%d_ovrTH", i); g.ovrTipHold         = GetF(kv, key, g.ovrTipHold);
                snprintf(key, sizeof(key), "g%d_ovrCP", i); g.ovrCollisionPadding= GetF(kv, key, g.ovrCollisionPadding);
                snprintf(key, sizeof(key), "g%d_ovrCF", i); g.ovrContactFriction = GetF(kv, key, g.ovrContactFriction);
                snprintf(key, sizeof(key), "g%d_ovrLA", i); g.ovrLimitAngle      = GetF(kv, key, g.ovrLimitAngle);
                snprintf(key, sizeof(key), "g%d_ovrLT", i); g.ovrLimitTipScale   = GetF(kv, key, g.ovrLimitTipScale);
                snprintf(key, sizeof(key), "g%d_ovrIN", i); g.ovrInertiaScale    = GetF(kv, key, g.ovrInertiaScale);
                snprintf(key, sizeof(key), "g%d_ovrWS", i); g.ovrWindStrength    = GetF(kv, key, g.ovrWindStrength);
                snprintf(key, sizeof(key), "g%d_ovrWF", i); g.ovrWindFreq        = GetF(kv, key, g.ovrWindFreq);
                // collide is not ovr-gated; default 1 = colliding.
                snprintf(key, sizeof(key), "g%d_col", i);   g.collide            = GetI(kv, key, 1) != 0;
                snprintf(key, sizeof(key), "g%d_ovrSk", i); g.ovrSpringStiffness = GetF(kv, key, g.ovrSpringStiffness);
                snprintf(key, sizeof(key), "g%d_ovrSd", i); g.ovrSpringDamping   = GetF(kv, key, g.ovrSpringDamping);
                snprintf(key, sizeof(key), "g%d_ovrSm", i); g.ovrSpringMass      = GetF(kv, key, g.ovrSpringMass);
                snprintf(key, sizeof(key), "g%d_ovrSg", i); g.ovrSpringGravity   = GetF(kv, key, g.ovrSpringGravity);
                snprintf(key, sizeof(key), "g%d_ovrSMD", i); g.ovrSpringMaxDisp  = GetF(kv, key, g.ovrSpringMaxDisp);
                // Each virtual-root child key defaults to its root's value, so a line without them drives the
                // children with the root's spring set, as it was authored.
                snprintf(key, sizeof(key), "g%d_ovrSCk", i);  g.ovrSpringChildStiffness = GetF(kv, key, g.ovrSpringStiffness);
                snprintf(key, sizeof(key), "g%d_ovrSCd", i);  g.ovrSpringChildDamping   = GetF(kv, key, g.ovrSpringDamping);
                snprintf(key, sizeof(key), "g%d_ovrSCm", i);  g.ovrSpringChildMass      = GetF(kv, key, g.ovrSpringMass);
                snprintf(key, sizeof(key), "g%d_ovrSCg", i);  g.ovrSpringChildGravity   = GetF(kv, key, g.ovrSpringGravity);
                snprintf(key, sizeof(key), "g%d_ovrSCMD", i); g.ovrSpringChildMaxDisp   = GetF(kv, key, g.ovrSpringChildMaxDisp);
                // World-motion inertia: absent = -1 = follow the component.
                snprintf(key, sizeof(key), "g%d_ovrWIL", i);  g.ovrWorldInertiaLocation = GetF(kv, key, -1.f);
                snprintf(key, sizeof(key), "g%d_ovrWIR", i);  g.ovrWorldInertiaRotation = GetF(kv, key, -1.f);
                snprintf(key, sizeof(key), "g%d_exc", i);
                int exc = GetI(kv, key, 0);
                if (exc < 0) exc = 0;
                if (exc > ChainGroupDef::MAX_EXCLUDE) exc = ChainGroupDef::MAX_EXCLUDE;
                g.excludeCount = exc;
                for (int j = 0; j < exc; ++j)
                {
                    snprintf(key, sizeof(key), "g%d_e%d", i, j);
                    snprintf(g.excludeBones[j], sizeof(g.excludeBones[j]), "%s", GetS(kv, key).c_str());
                }
            }

            // ---- Authored rest pose ----
            // Defaults are 1 / 1.f, not GetF / GetI's implicit 0: a missing rpEn must not disable an authored
            // pose, and a missing per-entry weight must not load every bone at weight 0 (ApplyRestPose skips
            // those, so the feature would vanish without a log).
            cp.restPoseEnabled = GetI(kv, "rpEn", 1) != 0;
            cp.restPoseWeight  = SanitizeWeight01(GetF(kv, "rpW",  1.f));
            int rc = GetI(kv, "rp", 0);
            if (rc < 0) rc = 0;
            if (rc > ChainPhysicsComponent::MAX_REST_POSE_BONES)
            {
                // Warn, unlike the groups clamp: [Mirror L/R] doubles the list in one click and [Capture current
                // pose] fills it wholesale, so hitting the cap is a real authoring outcome, not corruption.
                LOG_WARNING("ChainPhysics: rest-pose list truncated %d -> %d bones",
                            rc, ChainPhysicsComponent::MAX_REST_POSE_BONES);
                rc = ChainPhysicsComponent::MAX_REST_POSE_BONES;
            }
            cp.restPose.resize(static_cast<size_t>(rc));
            for (int i = 0; i < rc; ++i)
            {
                BoneRestPose& r = cp.restPose[i];
                char key[32];
                snprintf(key, sizeof(key), "r%d_b", i);
                snprintf(r.boneName, sizeof(r.boneName), "%s", GetS(kv, key).c_str());
                snprintf(key, sizeof(key), "r%d_q", i);
                {
                    // '_'-separated so the value survives ParseComponentLine
                    // untouched (same shape the Socket reader uses).
                    const std::string q = GetS(kv, key);
                    float c[4] = { 0.f, 0.f, 0.f, 1.f };
                    int   n = 0, parsed = 0;
                    size_t p = 0;
                    while (n < 4 && p <= q.size())
                    {
                        const size_t u = q.find('_', p);
                        const std::string tok = q.substr(p, u == std::string::npos ? std::string::npos : u - p);
                        if (!tok.empty()) { try { c[n] = std::stof(tok); ++parsed; } catch (...) { c[n] = (n == 3) ? 1.f : 0.f; } }
                        ++n;
                        if (u == std::string::npos) break;
                        p = u + 1;
                    }
                    // %.7g prints inf / nan / -nan(ind) for a poisoned value; those tokens have no space or '=' and
                    // pass the tokenizer into stof. Normalizing a non-finite or zero-length quaternion yields NaN,
                    // which reaches the skin matrix and access-violates SceneBVH::BuildRecursive.
                    const bool finite = std::isfinite(c[0]) && std::isfinite(c[1])
                                     && std::isfinite(c[2]) && std::isfinite(c[3]);
                    const float len2 = c[0]*c[0] + c[1]*c[1] + c[2]*c[2] + c[3]*c[3];
                    // parsed != 4 catches a TRUNCATED value ("0.3_0.1"): without
                    // it the tail keeps its initialiser and a half-written
                    // quaternion silently becomes a large bogus rotation instead
                    // of falling back to identity.
                    if (parsed != 4 || !finite || len2 < 1e-12f)
                        r.rot = { 0.f, 0.f, 0.f, 1.f };
                    else
                    {
                        const float inv = 1.f / std::sqrt(len2);
                        r.rot = { c[0]*inv, c[1]*inv, c[2]*inv, c[3]*inv };
                    }
                }
                // Weights get the same sanitising as the quaternion above: a
                // non-finite weight reaches XMQuaternionSlerp and produces the
                // exact NaN pose the quaternion guard exists to prevent.
                snprintf(key, sizeof(key), "r%d_w", i); r.weight  = SanitizeWeight01(GetF(kv, key, 1.f));
                snprintf(key, sizeof(key), "r%d_e", i); r.enabled = GetI(kv, key, 1) != 0;
            }

            w.AddComponent<ChainPhysicsComponent>(e, cp);
        }
    });

    // ==== FaceShadingComponent (INLINE) ====
    reg.Register(std::type_index(typeid(FaceShadingComponent)), {
        "FaceShading",
        [](World& w, Entity e) { return w.GetComponent<FaceShadingComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* fc = w.GetComponent<FaceShadingComponent>(e);
            if (!fc) return;
            ss << "  FaceShading: headBone=" << PercentEncode(fc->headBoneName)
               << " forwardAxis="   << fc->forwardAxis
               << " rightAxis="     << fc->rightAxis
               << " horizontalize=" << (fc->horizontalize ? 1 : 0)
               << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            FaceShadingComponent fc;
            fc.headBoneName  = GetS(kv, "headBone",      fc.headBoneName);
            fc.forwardAxis   = GetI(kv, "forwardAxis",   fc.forwardAxis);
            fc.rightAxis     = GetI(kv, "rightAxis",     fc.rightAxis);
            fc.horizontalize = GetI(kv, "horizontalize", fc.horizontalize ? 1 : 0) != 0;
            w.AddComponent<FaceShadingComponent>(e, fc);
        }
    });

    // ==== RigidBodyComponent (INLINE) ====
    reg.Register(std::type_index(typeid(RigidBodyComponent)), {
        "RigidBody",
        // A StaticMesh's derived body (DerivedColliderTag) is rebuilt from the asset, never saved.
        [](World& w, Entity e) { return w.GetComponent<RigidBodyComponent>(e) != nullptr &&
                                        !w.HasComponent<DerivedColliderTag>(e); },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* rb = w.GetComponent<RigidBodyComponent>(e);
            if (!rb) return;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "  RigidBody: motion=%u mass=%.4f linDamp=%.4f angDamp=%.4f"
                " friction=%.4f restitution=%.4f gravity=%.4f lockedAxes=%u\n",
                static_cast<uint32_t>(rb->motion),
                rb->mass, rb->linearDamping, rb->angularDamping,
                rb->friction, rb->restitution, rb->gravityFactor,
                static_cast<uint32_t>(rb->lockedAxes));
            ss << buf;
            // bodyId / lastBuilt* are runtime-only: PhysicsSystem creates the Jolt body on first sight.
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            RigidBodyComponent rb{};
            rb.motion         = static_cast<RigidBodyComponent::Motion>(
                                    GetI(kv, "motion", (int)RigidBodyComponent::Motion::Dynamic));
            rb.mass           = GetF(kv, "mass",        1.0f);
            rb.linearDamping  = GetF(kv, "linDamp",     0.05f);
            rb.angularDamping = GetF(kv, "angDamp",     0.05f);
            rb.friction       = GetF(kv, "friction",    0.5f);
            rb.restitution    = GetF(kv, "restitution", 0.0f);
            rb.gravityFactor  = GetF(kv, "gravity",     1.0f);
            // Absent -> 0: unconstrained 6-DOF.
            rb.lockedAxes     = static_cast<uint8_t>(GetU(kv, "lockedAxes", 0));
            rb.bodyId         = kInvalidPhysicsBodyId;
            w.AddComponent<RigidBodyComponent>(e, rb);
        }
    });

    // ==== ColliderComponent (INLINE) ====
    // Box / Sphere / Capsule fields are unconditionally written so prefab
    // editing in a text editor doesn't need to know the shape rules; the
    // deserializer reads each one with a sensible default if absent.
    reg.Register(std::type_index(typeid(ColliderComponent)), {
        "Collider",
        // A StaticMesh's derived collider (DerivedColliderTag) is rebuilt from the asset, never saved.
        [](World& w, Entity e) { return w.GetComponent<ColliderComponent>(e) != nullptr &&
                                        !w.HasComponent<DerivedColliderTag>(e); },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<ColliderComponent>(e);
            if (!c) return;
            // meshSec= is the mesh entry's SectionKey, which the reader resolves first; meshId= stays as the
            // hint (an older reader skips the key it does not know).
            std::string meshSec;
            if ((c->shape == ColliderComponent::Shape::Mesh || c->shape == ColliderComponent::Shape::ConvexHull) &&
                !c->meshLibPath.empty())
                if (const auto t = Resource::MeshLibKeyTableAt(c->meshLibPath))
                    if (const uint64_t k = t->KeyOf(c->meshLibMeshId))
                        meshSec = " meshSec=" + Resource::MeshKeyToHex(k);
            // A MeshAsset collider names the Mesh whose asset collision it is (an empty model = the
            // StaticMesh on the same entity).
            if (c->shape == ColliderComponent::Shape::MeshAsset)
                meshSec += " model=" + PercentEncode(c->assetModel) +
                           " meshKey=" + Resource::MeshKeyToHex(c->assetMeshKey);
            char buf[768];
            snprintf(buf, sizeof(buf),
                "  Collider: shape=%u"
                " halfExtents=%.4f_%.4f_%.4f radius=%.4f halfHeight=%.4f"
                " offset=%.4f_%.4f_%.4f"
                " rotEuler=%.4f_%.4f_%.4f"
                " meshPath=%s meshId=%u%s isTrigger=%u layer=%u\n",
                static_cast<uint32_t>(c->shape),
                c->halfExtents.x, c->halfExtents.y, c->halfExtents.z,
                c->radius, c->halfHeight,
                c->offset.x, c->offset.y, c->offset.z,
                c->rotationEulerDeg.x, c->rotationEulerDeg.y, c->rotationEulerDeg.z,
                PercentEncode(c->meshLibPath).c_str(),
                c->meshLibMeshId, meshSec.c_str(),
                c->isTrigger ? 1u : 0u,
                static_cast<uint32_t>(c->layer));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            ColliderComponent c{};
            c.shape         = static_cast<ColliderComponent::Shape>(
                                  GetI(kv, "shape", (int)ColliderComponent::Shape::Box));
            // halfExtents is one underscore-separated key ("x_y_z"), the LightData "color=" pattern: parse it
            // by hand.
            {
                auto it = kv.find("halfExtents");
                if (it != kv.end()) {
                    float x = 0.5f, y = 0.5f, z = 0.5f;
                    if (sscanf_s(it->second.c_str(), "%f_%f_%f", &x, &y, &z) >= 3) {
                        c.halfExtents = { x, y, z };
                    }
                }
            }
            c.radius        = GetF(kv, "radius",     0.5f);
            c.halfHeight    = GetF(kv, "halfHeight", 0.5f);
            // Absent offset -> {0,0,0}: the shape centered at the entity pivot.
            {
                auto it = kv.find("offset");
                if (it != kv.end()) {
                    float x = 0.f, y = 0.f, z = 0.f;
                    if (sscanf_s(it->second.c_str(), "%f_%f_%f", &x, &y, &z) >= 3) {
                        c.offset = { x, y, z };
                    }
                }
            }
            // Absent rotEuler -> identity rotation.
            {
                auto it = kv.find("rotEuler");
                if (it != kv.end()) {
                    float x = 0.f, y = 0.f, z = 0.f;
                    if (sscanf_s(it->second.c_str(), "%f_%f_%f", &x, &y, &z) >= 3) {
                        c.rotationEulerDeg = { x, y, z };
                    }
                }
            }
            c.meshLibPath   = GetS(kv, "meshPath",   "");
            c.meshLibMeshId = static_cast<uint32_t>(GetI(kv, "meshId", 0));
            // Shape::MeshAsset: the owning model and the MeshKey of its asset collision.
            c.assetModel = GetS(kv, "model", "");
            if (const auto it = kv.find("meshKey"); it != kv.end())
                Resource::MeshKeyFromHex(it->second, c.assetMeshKey);
            // The SectionKey names the entry: a re-import may have moved it, so it overrides the meshId hint.
            if (const auto it = kv.find("meshSec"); it != kv.end() && !c.meshLibPath.empty())
            {
                uint64_t key = 0;
                if (Resource::MeshKeyFromHex(it->second, key) && key != 0)
                    if (const auto t = Resource::MeshLibKeyTableAt(c.meshLibPath))
                    {
                        const int entry = t->Find(key);
                        if (entry < 0)
                            LOG_WARNING("Collider: '%s' has no section %s - meshId=%u (the hint) is used",
                                        c.meshLibPath.c_str(), it->second.c_str(), c.meshLibMeshId);
                        else if (static_cast<uint32_t>(entry) != c.meshLibMeshId)
                        {
                            LOG_WARNING("Collider: section %s of '%s' is entry %d now, not meshId=%u (the "
                                        "library was re-imported; saving refreshes the hint)",
                                        it->second.c_str(), c.meshLibPath.c_str(), entry, c.meshLibMeshId);
                            c.meshLibMeshId = static_cast<uint32_t>(entry);
                        }
                    }
            }
            // Absent -> a solid body.
            c.isTrigger     = GetI(kv, "isTrigger", 0) != 0;
            // Absent -> Auto (the motion-derived layer); an out-of-range value (corrupt or newer file) -> Auto
            // too.
            {
                const int layerVal = GetI(kv, "layer", 0);
                c.layer = (layerVal >= 0 &&
                           layerVal <= (int)ColliderComponent::CollisionLayer::Sensing)
                    ? static_cast<ColliderComponent::CollisionLayer>(layerVal)
                    : ColliderComponent::CollisionLayer::Auto;
            }
            w.AddComponent<ColliderComponent>(e, c);
        }
    });

    // ==== TagComponent (INLINE) ====
    // Gameplay identity tags, written as indexed percent-encoded tokens (t0=..t7=) so
    // Inspector-authored tags round-trip. No line -> no component, matching the lazy Engine.AddTag.
    reg.Register(std::type_index(typeid(TagComponent)), {
        "Tag",
        [](World& w, Entity e) { return w.GetComponent<TagComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* tc = w.GetComponent<TagComponent>(e);
            if (!tc) return;
            ss << "  Tag:";
            if (tc->count == 0) ss << " count=0";   // keep empty component alive
            for (uint32_t i = 0; i < tc->count && i < TagComponent::MAX; ++i)
                ss << " t" << i << "=" << PercentEncode(tc->tags[i]);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            TagComponent tc{};
            for (uint32_t i = 0; i < TagComponent::MAX; ++i)
            {
                auto it = kv.find("t" + std::to_string(i));
                if (it == kv.end()) continue;
                const std::string tag = PercentDecode(it->second);
                if (!tag.empty()) tc.Add(tag.c_str());
            }
            w.AddComponent<TagComponent>(e, tc);
        }
    });

    // ==== CharacterControllerComponent (INLINE) ====
    // Persists capsule + slope / gravity tuning. Runtime state (velocity, isGrounded, groundEntity,
    // timeInAir, bodyId, generation) is not serialized: PhysicsSystem creates the
    // JPH::CharacterVirtual on first sight from the pose LocalTransform already restored, and persisted
    // runtime fields would only fight that initialization.
    reg.Register(std::type_index(typeid(CharacterControllerComponent)), {
        "CharacterController",
        [](World& w, Entity e) { return w.GetComponent<CharacterControllerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* cc = w.GetComponent<CharacterControllerComponent>(e);
            if (!cc) return;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "  CharacterController: radius=%.4f halfHeight=%.4f stepHeight=%.4f"
                " maxSlope=%.4f skinWidth=%.4f mass=%.3f pushStrength=%.3f"
                " gravity=%.3f maxFallSpeed=%.3f jumpSpeed=%.3f"
                " groundAccel=%.3f groundDecel=%.3f airAccel=%.3f"
                " queryBody=%u\n",
                cc->capsuleRadius, cc->capsuleHalfHeight, cc->stepHeight,
                cc->maxSlopeRad, cc->skinWidth, cc->mass, cc->pushStrength,
                cc->gravity, cc->maxFallSpeed, cc->jumpSpeed,
                cc->groundAccel, cc->groundDecel, cc->airAccel,
                cc->queryBody ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CharacterControllerComponent cc{};
            // Absent -> false, what older scenes were authored against.
            cc.queryBody         = GetI(kv, "queryBody", 0) != 0;
            cc.capsuleRadius     = GetF(kv, "radius",       0.30f);
            cc.capsuleHalfHeight = GetF(kv, "halfHeight",   0.70f);
            cc.stepHeight        = GetF(kv, "stepHeight",   0.30f);
            cc.maxSlopeRad       = GetF(kv, "maxSlope",     0.7854f);
            cc.skinWidth         = GetF(kv, "skinWidth",    0.02f);
            cc.mass              = GetF(kv, "mass",         80.f);
            cc.pushStrength      = GetF(kv, "pushStrength", 1.0f);
            cc.gravity           = GetF(kv, "gravity",      -25.0f);
            cc.maxFallSpeed      = GetF(kv, "maxFallSpeed", 55.f);
            cc.jumpSpeed         = GetF(kv, "jumpSpeed",    7.5f);
            // Absent -> 0 = instant acceleration.
            cc.groundAccel       = GetF(kv, "groundAccel", 0.f);
            cc.groundDecel       = GetF(kv, "groundDecel", 0.f);
            cc.airAccel          = GetF(kv, "airAccel",    0.f);
            w.AddComponent<CharacterControllerComponent>(e, cc);
        }
    });

    // ==== PlayerComponent (INLINE) ====
    // Tuning + the camera-basis ref: cameraEntity is an AttachmentRef stored as camTargetGuid. An
    // older "camEntity=<idx>" is remapped via SceneLoadContext and re-stamped as a GUID, so the next
    // save is clean.
    reg.Register(std::type_index(typeid(PlayerComponent)), {
        "Player",
        [](World& w, Entity e) { return w.GetComponent<PlayerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* pc = w.GetComponent<PlayerComponent>(e);
            if (!pc) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  Player: walk=%.3f run=%.3f airControl=%.3f turnRate=%.3f"
                " facingMin=%.3f faceMode=%u coyote=%.3f jumpBuffer=%.3f",
                pc->walkSpeed, pc->runSpeed, pc->airControl,
                pc->turnRate, pc->facingMinSpeed,
                static_cast<unsigned>(pc->facingMode),
                pc->coyoteTime, pc->jumpBufferTime);
            ss << buf;
            WriteAttachmentRef(ss, "camTarget", w, pc->cameraEntity);
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            PlayerComponent pc{};
            pc.walkSpeed      = GetF(kv, "walk",        4.5f);
            pc.runSpeed       = GetF(kv, "run",         7.5f);
            pc.airControl     = GetF(kv, "airControl",  0.4f);
            pc.turnRate       = GetF(kv, "turnRate",   12.f);
            pc.facingMinSpeed = GetF(kv, "facingMin",   0.1f);
            // Absent -> FaceMovement.
            pc.facingMode     = static_cast<PlayerFacingMode>(
                                    std::clamp(GetI(kv, "faceMode", 0), 0, 2));
            pc.coyoteTime     = GetF(kv, "coyote",      0.10f);
            pc.jumpBufferTime = GetF(kv, "jumpBuffer",  0.10f);
            // Stale "runBand=" / "minFrac=" keys are ignored: the reader is key-driven. The key is
            // "camTargetGuid"; "camEntity" is the raw-index form.
            ReadAttachmentRef(kv, "camTarget", "camEntity", w, e, pc.cameraEntity);
            w.AddComponent<PlayerComponent>(e, pc);
        }
    });

    // ==== BrainComponent (INLINE) ====
    // Possession: which brain (Player / AI / None) drives this character's CharacterIntentComponent.
    // Absent = control by component presence.
    RegisterReflectedSerializer<BrainComponent>(reg, "Brain", Reflect::NumberStyle::Fixed4);

    // ==== CharacterAnimDriverComponent (INLINE) ====
    // Opt-in automatic animation selection (loco bridge + discrete gait
    // aliases + action layer). Alias strings must not contain spaces.
    reg.Register(std::type_index(typeid(CharacterAnimDriverComponent)), {
        "AnimDriver",
        [](World& w, Entity e) { return w.GetComponent<CharacterAnimDriverComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* d = w.GetComponent<CharacterAnimDriverComponent>(e);
            if (!d) return;
            ss << "  AnimDriver:";
            for (int i = 0; i < d->idleAliasCount
                         && i < CharacterAnimDriverComponent::MAX_VARIANTS; ++i)
                if (!d->idleAliases[i].empty())
                    ss << " idle" << i << '=' << d->idleAliases[i];
            for (int i = 0; i < d->walkAliasCount
                         && i < CharacterAnimDriverComponent::MAX_VARIANTS; ++i)
                if (!d->walkAliases[i].empty())
                    ss << " walk" << i << '=' << d->walkAliases[i];
            if (!d->walkBackAlias.empty())  ss << " back="  << d->walkBackAlias;
            if (!d->walkLeftAlias.empty())  ss << " left="  << d->walkLeftAlias;
            if (!d->walkRightAlias.empty()) ss << " right=" << d->walkRightAlias;
            if (!d->runAlias.empty())       ss << " run="   << d->runAlias;
            if (!d->airAlias.empty())       ss << " air="   << d->airAlias;
            char buf[200];
            snprintf(buf, sizeof(buf),
                " walkTh=%.3f runTh=%.3f gaitBlend=%.3f actionBlend=%.3f"
                " airGrace=%.3f idleReroll=%.3f",
                d->walkThreshold, d->runThreshold, d->gaitBlend,
                d->actionBlend, d->airTimeGrace, d->idleRerollSec);
            ss << buf;
            for (int i = 0; i < d->attackAliasCount
                         && i < CharacterAnimDriverComponent::MAX_ATTACKS; ++i)
                if (!d->attackAliases[i].empty())
                    ss << " attack" << i << '=' << d->attackAliases[i];
            for (int i = 0; i < d->dodgeAliasCount
                         && i < CharacterAnimDriverComponent::MAX_VARIANTS; ++i)
                if (!d->dodgeAliases[i].empty())
                    ss << " dodge" << i << '=' << d->dodgeAliases[i];
            if (!d->interactAlias.empty()) ss << " interact=" << d->interactAlias;
            if(d->graphSpeedParam!="Speed")ss<<" graphSpeedParam="<<PercentEncode(d->graphSpeedParam);
            if(d->graphDirectionParam!="Direction")ss<<" graphDirectionParam="<<PercentEncode(d->graphDirectionParam);
            if(d->graphGroundedParam!="IsGrounded")ss<<" graphGroundedParam="<<PercentEncode(d->graphGroundedParam);
            if(d->graphVerticalSpeedParam!="VerticalSpeed")ss<<" graphVerticalSpeedParam="<<PercentEncode(d->graphVerticalSpeedParam);
            if(d->graphAirTimeParam!="AirTime")ss<<" graphAirTimeParam="<<PercentEncode(d->graphAirTimeParam);
            if(d->graphTurnRateParam!="TurnRate")ss<<" graphTurnRateParam="<<PercentEncode(d->graphTurnRateParam);
            if(d->graphMovingParam!="IsMoving")ss<<" graphMovingParam="<<PercentEncode(d->graphMovingParam);
            if(d->graphGaitParam!="Gait")ss<<" graphGaitParam="<<PercentEncode(d->graphGaitParam);
            if(d->graphActionSlot!="DefaultSlot")ss<<" graphActionSlot="<<PercentEncode(d->graphActionSlot);
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CharacterAnimDriverComponent d{};
            auto readList = [&kv](const char* prefix, std::string* dst,
                                  int cap) -> int
            {
                int count = 0;
                for (int i = 0; i < cap; ++i)
                {
                    char key[16];
                    snprintf(key, sizeof(key), "%s%d", prefix, i);
                    std::string a = GetS(kv, key, "");
                    if (a.empty()) continue;
                    dst[count++] = std::move(a);
                }
                return count;
            };
            int nIdle = readList("idle", d.idleAliases,
                                 CharacterAnimDriverComponent::MAX_VARIANTS);
            if (nIdle > 0) d.idleAliasCount = nIdle;   // else keep default "IDLE"
            int nWalk = readList("walk", d.walkAliases,
                                 CharacterAnimDriverComponent::MAX_VARIANTS);
            if (nWalk > 0) d.walkAliasCount = nWalk;
            d.walkBackAlias  = GetS(kv, "back",  "");
            d.walkLeftAlias  = GetS(kv, "left",  "");
            d.walkRightAlias = GetS(kv, "right", "");
            d.runAlias       = GetS(kv, "run",  "RUN");
            d.airAlias       = GetS(kv, "air",  "");
            d.walkThreshold  = GetF(kv, "walkTh",      0.15f);
            d.runThreshold   = GetF(kv, "runTh",       5.0f);
            d.gaitBlend      = GetF(kv, "gaitBlend",   0.20f);
            d.actionBlend    = GetF(kv, "actionBlend", 0.10f);
            d.airTimeGrace   = GetF(kv, "airGrace",    0.12f);
            d.idleRerollSec  = GetF(kv, "idleReroll",  0.f);
            d.attackAliasCount = readList("attack", d.attackAliases,
                                          CharacterAnimDriverComponent::MAX_ATTACKS);
            d.dodgeAliasCount  = readList("dodge",  d.dodgeAliases,
                                          CharacterAnimDriverComponent::MAX_VARIANTS);
            d.interactAlias    = GetS(kv, "interact", "");
            d.graphSpeedParam=GetS(kv,"graphSpeedParam","Speed");
            d.graphDirectionParam=GetS(kv,"graphDirectionParam","Direction");
            d.graphGroundedParam=GetS(kv,"graphGroundedParam","IsGrounded");
            d.graphVerticalSpeedParam=GetS(kv,"graphVerticalSpeedParam","VerticalSpeed");
            d.graphAirTimeParam=GetS(kv,"graphAirTimeParam","AirTime");
            d.graphTurnRateParam=GetS(kv,"graphTurnRateParam","TurnRate");
            d.graphMovingParam=GetS(kv,"graphMovingParam","IsMoving");
            d.graphGaitParam=GetS(kv,"graphGaitParam","Gait");
            d.graphActionSlot=GetS(kv,"graphActionSlot","DefaultSlot");

            w.AddComponent<CharacterAnimDriverComponent>(e, d);
        }
    });

    // ==== PerObjectShadowComponent (INLINE) ====
    // Opt-in tag for the high-res per-object directional shadow.
    reg.Register(std::type_index(typeid(PerObjectShadowComponent)), {
        "PerObjectShadow",
        [](World& w, Entity e) { return w.GetComponent<PerObjectShadowComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<PerObjectShadowComponent>(e);
            if (!c) return;
            char buf[96];
            snprintf(buf, sizeof(buf), "  PerObjectShadow: radiusScale=%.3f\n", c->radiusScale);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            PerObjectShadowComponent c{};
            c.radiusScale = GetF(kv, "radiusScale", 1.0f);
            w.AddComponent<PerObjectShadowComponent>(e, c);
        }
    });

    // ==== DissolveComponent (INLINE) ====
    // Per-entity noise dissolve: authored look fields only. The runtime lerp state (amount / target /
    // duration / destroyOnComplete) is not saved, so a scene saved mid-dissolve loads intact (transient
    // state does not round-trip, as with LifetimeComponent).
    reg.Register(std::type_index(typeid(DissolveComponent)), {
        "Dissolve",
        [](World& w, Entity e) { return w.GetComponent<DissolveComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<DissolveComponent>(e);
            if (!c) return;
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "  Dissolve: edgeR=%.3f edgeG=%.3f edgeB=%.3f "
                     "edgeWidth=%.3f edgeIntensity=%.3f noiseScale=%.3f\n",
                     c->edgeColor.x, c->edgeColor.y, c->edgeColor.z,
                     c->edgeWidth, c->edgeIntensity, c->noiseScale);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            DissolveComponent c{};
            c.edgeColor.x   = GetF(kv, "edgeR", 1.0f);
            c.edgeColor.y   = GetF(kv, "edgeG", 0.45f);
            c.edgeColor.z   = GetF(kv, "edgeB", 0.1f);
            c.edgeWidth     = GetF(kv, "edgeWidth", 0.08f);
            c.edgeIntensity = GetF(kv, "edgeIntensity", 6.0f);
            c.noiseScale    = GetF(kv, "noiseScale", 1.0f);
            w.AddComponent<DissolveComponent>(e, c);
        }
    });

    // ==== DecalComponent (INLINE) ====
    // Per-instance projection-box state only. The decal look persists through the entity's material
    // blocks (MaterialRef / .imat with domain=Decal + MaterialOverride). The DecalMaterialLibrary link
    // is a volatile runtime shared_ptr (the library is wiped on OnWorldClear, names do not round-trip)
    // and is not serialized: author a decal with a Decal-domain material for full save / load.
    // fadeAlpha is transient (DecalLifetimeSystem drives it).
    reg.Register(std::type_index(typeid(DecalComponent)), {
        "Decal",
        [](World& w, Entity e) { return w.GetComponent<DecalComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<DecalComponent>(e);
            if (!c) return;
            char buf[224];
            snprintf(buf, sizeof(buf),
                     "  Decal: lifetime=%.3f fadeOut=%.3f destroyOnExpire=%d "
                     "tintR=%.4f tintG=%.4f tintB=%.4f tintA=%.4f\n",
                     c->lifetime, c->fadeOutDuration,
                     c->destroyEntityOnExpire ? 1 : 0,
                     c->tintOverride.x, c->tintOverride.y,
                     c->tintOverride.z, c->tintOverride.w);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            DecalComponent c{};
            c.lifetime              = GetF(kv, "lifetime", -1.0f);
            c.fadeOutDuration       = GetF(kv, "fadeOut", 0.5f);
            c.destroyEntityOnExpire = GetF(kv, "destroyOnExpire", 1.0f) != 0.0f;
            c.tintOverride.x        = GetF(kv, "tintR", 0.0f);
            c.tintOverride.y        = GetF(kv, "tintG", 0.0f);
            c.tintOverride.z        = GetF(kv, "tintB", 0.0f);
            c.tintOverride.w        = GetF(kv, "tintA", 0.0f);
            w.AddComponent<DecalComponent>(e, c);
        }
    });

    // ==== HairShadowCasterComponent (INLINE) ====
    // Opt-in tag for the hair (bangs -> face) shadow. Tag the bangs entity; NPR face pixels consume
    // the screen mark / shadow map in Lighting.ps.
    reg.Register(std::type_index(typeid(HairShadowCasterComponent)), {
        "HairShadowCaster",
        [](World& w, Entity e) { return w.GetComponent<HairShadowCasterComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<HairShadowCasterComponent>(e);
            if (!c) return;
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "  HairShadowCaster: mode=%d strength=%.3f fitScale=%.3f"
                     " screenOffsetX=%.3f screenOffsetY=%.3f depthPush=%.3f\n",
                     c->mode, c->strength, c->fitScale,
                     c->screenOffsetX, c->screenOffsetY, c->depthPush);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            HairShadowCasterComponent c{};
            c.mode          = static_cast<int>(GetF(kv, "mode", 0.0f));
            c.strength      = GetF(kv, "strength", 1.0f);
            c.fitScale      = GetF(kv, "fitScale", 1.0f);
            c.screenOffsetX = GetF(kv, "screenOffsetX", 1.0f);
            c.screenOffsetY = GetF(kv, "screenOffsetY", 1.0f);
            c.depthPush     = GetF(kv, "depthPush", 0.5f);
            w.AddComponent<HairShadowCasterComponent>(e, c);
        }
    });

    // ==== AnimFXGateComponent (INLINE) ====
    // Opt-in marker on a weapon prefab root: its Trail / ParticleEmitter group is armed by FXGate
    // notify intervals on the attack clip instead of being always live. lastApplied / hasApplied are
    // runtime latches; saving them would restore a stale "already off" and skip the first apply.
    RegisterReflectedSerializer<AnimFXGateComponent>(reg, "AnimFXGate", Reflect::NumberStyle::Fixed4);

    // ==== FaceOverlayComponent (INLINE) ====
    // Opt-in tag for the brows/lashes-through-hair overlay. Tag the EYEBROW /
    // EYELASH entities; FaceOverlayPass re-draws them where hair covers them.
    reg.Register(std::type_index(typeid(FaceOverlayComponent)), {
        "FaceOverlay",
        [](World& w, Entity e) { return w.GetComponent<FaceOverlayComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* c = w.GetComponent<FaceOverlayComponent>(e);
            if (!c) return;
            char buf[96];
            snprintf(buf, sizeof(buf), "  FaceOverlay: alpha=%.3f maxHairDistance=%.3f\n",
                     c->alpha, c->maxHairDistance);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            FaceOverlayComponent c{};
            c.alpha           = GetF(kv, "alpha", 0.35f);
            c.maxHairDistance = GetF(kv, "maxHairDistance", 0.05f);
            w.AddComponent<FaceOverlayComponent>(e, c);
        }
    });

    reg.Register(std::type_index(typeid(AIBehaviorComponent)), {
        "AIBehavior",
        [](World& w,Entity e){return w.HasComponent<AIBehaviorComponent>(e);},
        [](World& w,Entity e,std::ostringstream& ss){
            const auto* c=w.GetComponent<AIBehaviorComponent>(e);if(!c)return;
            ss<<"  AIBehavior: enabled="<<c->enabled<<" targetMode="<<c->targetMode
                <<" fixedRequiresSight="<<c->fixedRequiresSight<<" memorySeconds="<<c->memorySeconds
                <<" leashRadius="<<c->leashRadius<<" trackDuringMemory="<<c->trackDuringMemory
                <<" patrolMode="<<c->patrolMode<<" arriveRadius="<<c->arriveRadius
                <<" waitSeconds="<<c->waitSeconds<<" legTimeout="<<c->legTimeout<<" pointCount="<<c->patrolPoints.size();
            WriteAttachmentRef(ss,"fixedTarget",w,c->fixedTarget);
            for(size_t i=0;i<c->patrolPoints.size();++i)WriteAttachmentRef(ss,("point"+std::to_string(i)).c_str(),w,c->patrolPoints[i]);
            ss<<'\n';
        },
        [](World& w,Entity e,const KVMap& kv,Resource::AssetManager*){
            AIBehaviorComponent c;
            c.enabled=GetI(kv,"enabled",1)!=0;c.targetMode=std::clamp(GetI(kv,"targetMode",1),0,3);
            c.fixedRequiresSight=GetI(kv,"fixedRequiresSight",1)!=0;c.memorySeconds=std::clamp(GetF(kv,"memorySeconds",5),0.f,120.f);
            c.leashRadius=std::clamp(GetF(kv,"leashRadius",30),0.f,10000.f);c.trackDuringMemory=GetI(kv,"trackDuringMemory",0)!=0;
            c.patrolMode=std::clamp(GetI(kv,"patrolMode",0),0,2);c.arriveRadius=std::clamp(GetF(kv,"arriveRadius",.4f),.01f,100.f);
            c.waitSeconds=std::clamp(GetF(kv,"waitSeconds",1.5f),0.f,120.f);c.legTimeout=std::clamp(GetF(kv,"legTimeout",10),.1f,600.f);
            ReadAttachmentRef(kv,"fixedTarget",nullptr,w,e,c.fixedTarget);
            c.patrolPoints.resize(std::clamp(GetI(kv,"pointCount",0),0,256));
            for(size_t i=0;i<c.patrolPoints.size();++i)ReadAttachmentRef(kv,("point"+std::to_string(i)).c_str(),nullptr,w,e,c.patrolPoints[i]);
            w.AddComponent<AIBehaviorComponent>(e,std::move(c));
        }
    });

    // ==== AIComponent (INLINE) ====
    // Persists the tree path + tickInterval + enabled flag. BTAsset / BTInstance are runtime-only:
    // AISystem::TickEntity resolves treePath -> BTAsset via AcquireTree on its first tick.
    reg.Register(std::type_index(typeid(AIComponent)), {
        "AI",
        [](World& w, Entity e) { return w.GetComponent<AIComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ai = w.GetComponent<AIComponent>(e);
            if (!ai) return;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "  AI: enabled=%u tickInterval=%.4f treePath=%s\n",
                ai->enabled ? 1u : 0u,
                ai->tickInterval,
                PercentEncode(ai->treePath).c_str());
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AIComponent ai;
            ai.enabled       = GetI(kv, "enabled", 1) != 0;
            ai.tickInterval  = GetF(kv, "tickInterval", 0.1f);
            ai.treePath      = GetS(kv, "treePath", "");
            // tree / instance stay null: AISystem::TickEntity hydrates them the first time it sees a
            // non-empty path.
            w.AddComponent<AIComponent>(e, std::move(ai));
        }
    });

    // ==== BlackboardComponent (INLINE) ====
    // The BB is runtime-only AI scratch state (chase timers, the wander target, the spawn timestamp,
    // animation-variant picks); saving it would poison every later session with whatever was live when
    // the prefab / world was saved. AISystem creates an empty BB on the first BT tick, so both
    // directions are disabled:
    //   * has() -> always false: no save writes a BB
    //   * reader -> no-op: an old save's "Blackboard:" line is discarded
    // For persistent AI memory, add a separate "PersistentBB" component so transient keys still get
    // wiped.
    reg.Register(std::type_index(typeid(BlackboardComponent)), {
        "Blackboard",
        [](World& /*w*/, Entity /*e*/) {
            return false;
        },
        [](World& /*w*/, Entity /*e*/, std::ostringstream& /*ss*/) {
            // Unreachable: has() is false, so nothing is written.
        },
        [](World& /*w*/, Entity /*e*/, const KVMap& /*kv*/, Resource::AssetManager*) {
            // Intentionally empty: a BB is never loaded from a file; AISystem creates a fresh one on the
            // first BT tick.
            return;
        }
    });

    reg.Register(std::type_index(typeid(IKRigComponent)), {
        "IKRig",
        [](World& w,Entity e){return w.GetComponent<IKRigComponent>(e)!=nullptr;},
        [](World& w,Entity e,std::ostringstream& ss){
            auto* rig=w.GetComponent<IKRigComponent>(e);if(!rig)return;
            ss << "  IKRig:";
            VisitIKRigAuthoring(*rig,[&](const char* key,const auto& value,const auto& def){
                if(value==def)return;
                ss << " " << key << "=";
                using T=std::decay_t<decltype(value)>;
                if constexpr(std::is_same_v<T,std::string>)ss<<PercentEncode(value);
                else if constexpr(std::is_enum_v<T>)ss<<int(value);
                else ss<<value;
            });
            ss<<"\n";
        },
        [](World& w,Entity e,const KVMap& kv,Resource::AssetManager*){
            IKRigComponent rig;
            VisitIKRigAuthoring(rig,[&](const char* key,auto& value,const auto& def){
                using T=std::decay_t<decltype(value)>;
                if constexpr(std::is_same_v<T,std::string>)value=PercentDecode(GetS(kv,key,def));
                else if constexpr(std::is_enum_v<T>)value=static_cast<T>(GetI(kv,key,int(def)));
                else if constexpr(std::is_same_v<T,bool>)value=GetI(kv,key,def)!=0;
                else if constexpr(std::is_integral_v<T>)value=static_cast<T>(GetI(kv,key,int(def)));
                else value=GetF(kv,key,def);
            });
            // An older line's AnimGraph IKRig "goals" JSON is read into the Custom goal slots.
            if(kv.contains("goals"))try {
                size_t slot=size_t(IKGoalSlot::Custom0);
                for(const auto& j:nlohmann::json::parse(GetS(kv,"goals","[]"))) {
                    if(slot>=kIKGoalCount)break;
                    auto& goal=rig.goals[slot++];goal.enabled=true;goal.curveDriven=true;
                    goal.rootBone=j.at("root");goal.midBone=j.at("mid");goal.endBone=j.at("end");
                    goal.curve=j.value("curve","");goal.weight=j.value("weight",1.f);
                    const auto t=j.at("target"),p=j.at("pole");
                    goal.curveTarget={t.at(0),t.at(1),t.at(2)};goal.polePoint={p.at(0),p.at(1),p.at(2)};
                    goal.refMode=IKRefMode::Virtual;goal.solver=IKSolverKind::TwoBone;goal.poleMode=IKPoleMode::WorldPoint;goal.softness=0.f;
                }
            }catch(const std::exception&){LOG_WARNING("IKRig: invalid legacy goals");}
            w.AddComponent<IKRigComponent>(e,std::move(rig));
        }
    });

    // ==== HeadTrackComponent (INLINE) ====
    // Only the authored tunables travel; the resolved bone index + smoothing
    // state are runtime and re-derived on the first tick after load.
    reg.Register(std::type_index(typeid(HeadTrackComponent)), {
        "HeadTrack",
        [](World& w, Entity e) { return w.GetComponent<HeadTrackComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* ht = w.GetComponent<HeadTrackComponent>(e);
            if (!ht) return;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "  HeadTrack: enabled=%u headBone=%s forwardAxis=%d upAxis=%d"
                " yawLimit=%.4f pitchLimit=%.4f weight=%.4f smooth=%.4f"
                " maxTurnSpeed=%.4f turnAccel=%.4f returnSpeed=%.4f",
                ht->enabled ? 1u : 0u, PercentEncode(ht->headBoneName).c_str(),
                ht->forwardAxis, ht->upAxis,
                ht->yawLimitDeg, ht->pitchLimitDeg, ht->weight, ht->smoothingSpeed,
                ht->maxTurnSpeedDeg, ht->turnAccelDeg, ht->returnSpeed);
            ss << buf;
            // Look target: a GUID entity ref ("targetGuid=..."); empty = the main camera.
            WriteAttachmentRef(ss, "target", w, ht->targetRef);
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            HeadTrackComponent ht;
            ht.enabled        = GetI(kv, "enabled",     1) != 0;
            ht.headBoneName   = GetS(kv, "headBone",    ht.headBoneName);
            ht.forwardAxis    = GetI(kv, "forwardAxis", ht.forwardAxis);
            ht.upAxis         = GetI(kv, "upAxis",      ht.upAxis);
            ht.yawLimitDeg    = GetF(kv, "yawLimit",    ht.yawLimitDeg);
            ht.pitchLimitDeg  = GetF(kv, "pitchLimit",  ht.pitchLimitDeg);
            ht.weight         = GetF(kv, "weight",       ht.weight);
            ht.smoothingSpeed = GetF(kv, "smooth",       ht.smoothingSpeed);
            ht.maxTurnSpeedDeg= GetF(kv, "maxTurnSpeed", ht.maxTurnSpeedDeg);
            ht.turnAccelDeg   = GetF(kv, "turnAccel",    ht.turnAccelDeg);
            ht.returnSpeed    = GetF(kv, "returnSpeed",  ht.returnSpeed);
            ReadAttachmentRef(kv, "target", /*legacyKey*/ nullptr, w, e, ht.targetRef);
            w.AddComponent<HeadTrackComponent>(e, ht);
        }
    });

    // ==== FacialAnimationComponent (INLINE) ====
    // Morph-target (blend shape) control: auto-blink, expressions, visemes. Not FaceShading (SDF face
    // shadows), a different component and tag.
    // Only the authored tunables travel. Channels, resolved mesh slots, the blink state machine and
    // resolvedAssetIndex are runtime, re-derived on the first tick after load (a stale slot from
    // another rig would be worse than a rescan); runtimeSeeded stays false on a fresh component, which
    // makes FacialAnimationSystem re-seed the RNG and re-resolve everything.
    // Names use PercentEncode (escapes only '%' and ' '), not PercentEncodeStrict: morph names are
    // UTF-8 Japanese, the tokenizer passes high bytes through verbatim, and scene files already carry
    // a Japanese headBone=; strict encoding would mangle them all for no gain. The variable-length
    // authored list uses the AnimationSet indexed-key idiom (an=<count>, then anName<i> / anW<i>),
    // since a KVMap cannot hold repeated keys.
    reg.Register(std::type_index(typeid(FacialAnimationComponent)), {
        "FacialAnimation",
        [](World& w, Entity e) { return w.GetComponent<FacialAnimationComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* fa = w.GetComponent<FacialAnimationComponent>(e);
            if (!fa) return;
            ss << "  FacialAnimation:"
               << " enabled="   << (fa->enabled ? 1 : 0)
               << " autoBlink=" << (fa->autoBlinkEnabled ? 1 : 0)
               << " blinkMorph=" << PercentEncode(fa->blinkMorph);
            char buf[192];
            snprintf(buf, sizeof(buf),
                     " blinkMin=%.4f blinkMax=%.4f blinkClose=%.4f"
                     " blinkHold=%.4f blinkOpen=%.4f blinkMaxW=%.4f",
                     fa->blinkIntervalMinSec, fa->blinkIntervalMaxSec,
                     fa->blinkCloseSec, fa->blinkHoldSec,
                     fa->blinkOpenSec, fa->blinkMaxWeight);
            ss << buf;
            static const char* kVowelKey[FacialAnimationComponent::kVowelCount] = {
                "vowelA", "vowelI", "vowelU", "vowelE", "vowelO"
            };
            for (int v = 0; v < FacialAnimationComponent::kVowelCount; ++v)
                ss << ' ' << kVowelKey[v] << '=' << PercentEncode(fa->visemeMorph[v]);

            // The whole (sparse) Authored list, up to kMaxAuthored entries (older files hold at most 8). An
            // entry at weight 0 is no entry (the reader drops it too).
            std::vector<const FacialAnimationComponent::AuthoredEntry*> kept;
            for (const FacialAnimationComponent::AuthoredEntry& a : fa->authored)
                if (!a.name.empty() && a.weight > 0.f &&
                    kept.size() < static_cast<std::size_t>(FacialAnimationComponent::kMaxAuthored))
                    kept.push_back(&a);
            ss << " an=" << kept.size();
            for (std::size_t i = 0; i < kept.size(); ++i)
            {
                ss << " anName" << i << '=' << PercentEncode(kept[i]->name)
                   << " anW"    << i << '=' << kept[i]->weight;
            }
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            FacialAnimationComponent fa;   // defaults carry the MMD blink (mabataki) and a/i/u/e/o viseme names
            fa.enabled             = GetI(kv, "enabled",   1) != 0;
            fa.autoBlinkEnabled    = GetI(kv, "autoBlink", 1) != 0;
            Facial::CopyName(fa.blinkMorph, GetS(kv, "blinkMorph", fa.blinkMorph).c_str());
            fa.blinkIntervalMinSec = GetF(kv, "blinkMin",   fa.blinkIntervalMinSec);
            fa.blinkIntervalMaxSec = GetF(kv, "blinkMax",   fa.blinkIntervalMaxSec);
            fa.blinkCloseSec       = GetF(kv, "blinkClose", fa.blinkCloseSec);
            fa.blinkHoldSec        = GetF(kv, "blinkHold",  fa.blinkHoldSec);
            fa.blinkOpenSec        = GetF(kv, "blinkOpen",  fa.blinkOpenSec);
            fa.blinkMaxWeight      = GetF(kv, "blinkMaxW",  fa.blinkMaxWeight);

            static const char* kVowelKey[FacialAnimationComponent::kVowelCount] = {
                "vowelA", "vowelI", "vowelU", "vowelE", "vowelO"
            };
            for (int v = 0; v < FacialAnimationComponent::kVowelCount; ++v)
                Facial::CopyName(fa.visemeMorph[v],
                                 GetS(kv, kVowelKey[v], fa.visemeMorph[v]).c_str());

            // Through Facial::SetAuthored: a blank name is skipped, a repeated one
            // keeps its last weight, a zero weight is no entry - the list's rules.
            const int an = std::clamp(GetI(kv, "an", 0), 0,
                                      FacialAnimationComponent::kMaxAuthored);
            for (int i = 0; i < an; ++i)
            {
                char key[24];
                snprintf(key, sizeof(key), "anName%d", i);
                const std::string nm = GetS(kv, key, "");
                if (nm.empty()) continue;
                snprintf(key, sizeof(key), "anW%d", i);
                Facial::SetAuthored(fa, nm.c_str(), SanitizeWeight01(GetF(kv, key, 0.f)));
            }
            w.AddComponent<FacialAnimationComponent>(e, fa);
        }
    });

    // ==== LocomotionComponent (INLINE) ====
    // Blend-space samples serialize as (path, speed) pairs; runtime clip
    // indices / phase are re-resolved after load (ResolvePendingBinds).
    reg.Register(std::type_index(typeid(LocomotionComponent)), {
        "Locomotion",
        [](World& w, Entity e) { return w.GetComponent<LocomotionComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* lc = w.GetComponent<LocomotionComponent>(e);
            if (!lc) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  Locomotion: enabled=%u mode=%u speed=%.4f dir=%.4f damping=%.4f"
                " playRate=%.4f n=%d",
                lc->enabled ? 1u : 0u, static_cast<unsigned>(lc->mode),
                lc->speed, lc->dirDeg, lc->damping, lc->playRate, lc->sampleCount);
            ss << buf;
            for (int i = 0; i < lc->sampleCount &&
                            i < static_cast<int>(LocomotionComponent::MAX_SAMPLES); ++i)
            {
                snprintf(buf, sizeof(buf), " p%d=%s v%d=%.4f a%d=%.4f",
                         i, PercentEncode(lc->samples[i].animPath).c_str(),
                         i, lc->samples[i].speed,
                         i, lc->samples[i].angleDeg);
                ss << buf;
            }
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            LocomotionComponent lc;
            lc.enabled     = GetI(kv, "enabled", 1) != 0;
            lc.mode        = static_cast<uint8_t>(GetI(kv, "mode", 0));
            lc.speed       = GetF(kv, "speed",    lc.speed);
            lc.dirDeg      = GetF(kv, "dir",      lc.dirDeg);
            lc.damping     = GetF(kv, "damping",  lc.damping);
            lc.playRate    = GetF(kv, "playRate", lc.playRate);
            lc.sampleCount = std::clamp(GetI(kv, "n", 0), 0,
                                        static_cast<int>(LocomotionComponent::MAX_SAMPLES));
            for (int i = 0; i < lc.sampleCount; ++i)
            {
                char pk[8], vk[8], ak[8];
                snprintf(pk, sizeof(pk), "p%d", i);
                snprintf(vk, sizeof(vk), "v%d", i);
                snprintf(ak, sizeof(ak), "a%d", i);
                lc.samples[i].animPath = GetS(kv, pk, "");
                lc.samples[i].speed    = GetF(kv, vk, 0.f);
                lc.samples[i].angleDeg = GetF(kv, ak, 0.f);
            }
            w.AddComponent<LocomotionComponent>(e, lc);
        }
    });

    // ==== LocomotionTransitionComponent (INLINE) ====
    // Start/stop/pivot/turn one-shots layered over the blend space. Same
    // (path, angle) authoring shape as LocomotionComponent plus a kind.
    reg.Register(std::type_index(typeid(LocomotionTransitionComponent)), {
        "LocoTransitions",
        [](World& w, Entity e) { return w.GetComponent<LocomotionTransitionComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* tc = w.GetComponent<LocomotionTransitionComponent>(e);
            if (!tc) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  LocoTransitions: enabled=%u startSpeed=%.4f stopSpeed=%.4f"
                " pivotAngle=%.4f turnRate=%.4f blendIn=%.4f blendOut=%.4f"
                " cooldown=%.4f releaseLead=%.4f n=%d",
                tc->enabled ? 1u : 0u, tc->startSpeed, tc->stopSpeed,
                tc->pivotAngle, tc->turnRate, tc->blendIn, tc->blendOut,
                tc->cooldown, tc->releaseLead, tc->count);
            ss << buf;
            for (int i = 0; i < tc->count &&
                            i < LocomotionTransitionComponent::MAX_TRANSITIONS; ++i)
            {
                snprintf(buf, sizeof(buf),
                         " p%d=%s k%d=%u a%d=%.4f s%d=%.4f m%d=%.4f l%d=%u",
                         i, PercentEncode(tc->transitions[i].animPath).c_str(),
                         i, static_cast<unsigned>(tc->transitions[i].kind),
                         i, tc->transitions[i].angleDeg,
                         i, tc->transitions[i].speed,
                         i, tc->transitions[i].maxDuration,
                         i, tc->transitions[i].looping ? 1u : 0u);
                ss << buf;
            }
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            LocomotionTransitionComponent tc;
            tc.enabled     = GetI(kv, "enabled", 1) != 0;
            tc.startSpeed  = GetF(kv, "startSpeed",  tc.startSpeed);
            tc.stopSpeed   = GetF(kv, "stopSpeed",   tc.stopSpeed);
            tc.pivotAngle  = GetF(kv, "pivotAngle",  tc.pivotAngle);
            tc.turnRate    = GetF(kv, "turnRate",    tc.turnRate);
            tc.blendIn     = GetF(kv, "blendIn",     tc.blendIn);
            tc.blendOut    = GetF(kv, "blendOut",    tc.blendOut);
            tc.cooldown    = GetF(kv, "cooldown",    tc.cooldown);
            tc.releaseLead = GetF(kv, "releaseLead", tc.releaseLead);
            tc.count = std::clamp(GetI(kv, "n", 0), 0,
                                  LocomotionTransitionComponent::MAX_TRANSITIONS);
            for (int i = 0; i < tc.count; ++i)
            {
                char pk[8], kk[8], ak[8], sk[8], mk[8], lk[8];
                snprintf(pk, sizeof(pk), "p%d", i);
                snprintf(kk, sizeof(kk), "k%d", i);
                snprintf(ak, sizeof(ak), "a%d", i);
                snprintf(sk, sizeof(sk), "s%d", i);
                snprintf(mk, sizeof(mk), "m%d", i);
                snprintf(lk, sizeof(lk), "l%d", i);
                tc.transitions[i].animPath    = GetS(kv, pk, "");
                tc.transitions[i].kind        = static_cast<uint8_t>(
                                                    std::clamp(GetI(kv, kk, 0), 0, 6));
                tc.transitions[i].angleDeg    = GetF(kv, ak, 0.f);
                tc.transitions[i].speed       = GetF(kv, sk, 1.f);
                tc.transitions[i].maxDuration = GetF(kv, mk, 0.f);
                tc.transitions[i].looping     = GetI(kv, lk, 0) != 0;
            }
            w.AddComponent<LocomotionTransitionComponent>(e, tc);
        }
    });

    // ==== AnimLayerComponent (INLINE) ====
    reg.Register(std::type_index(typeid(AnimLayerComponent)), {
        "AnimLayers",
        [](World& w, Entity e) { return w.GetComponent<AnimLayerComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* lc = w.GetComponent<AnimLayerComponent>(e);
            if (!lc) return;
            char buf[320];
            snprintf(buf, sizeof(buf), "  AnimLayers: n=%d", lc->layerCount);
            ss << buf;
            for (int i = 0; i < lc->layerCount && i < AnimLayerComponent::MAX_LAYERS; ++i)
            {
                const AnimLayer& L = lc->layers[i];
                // The two variable-length fields (path, root bone) are streamed, not snprintf'd: in buf a long
                // .ianim path plus a percent-encoded UTF-8 bone name would overflow and truncate the mask root,
                // the layer would load with an empty customRootBone, and BuildBoneMask would fall back to full
                // body (an upper-body layer taking over the whole character).
                snprintf(buf, sizeof(buf),
                    " w%d=%.4f s%d=%.4f loop%d=%u play%d=%u mask%d=%u",
                    i, L.weight, i, L.speed,
                    i, L.looping ? 1u : 0u, i, L.playing ? 1u : 0u,
                    i, static_cast<unsigned>(L.maskMode));
                ss << " p" << i << '=' << PercentEncode(L.animPath)
                   << buf
                   << " root" << i << '=' << PercentEncode(L.customRootBone);
            }
            ss << '\n';
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AnimLayerComponent lc;
            lc.layerCount = std::clamp(GetI(kv, "n", 0), 0, AnimLayerComponent::MAX_LAYERS);
            for (int i = 0; i < lc.layerCount; ++i)
            {
                AnimLayer& L = lc.layers[i];
                char k[16];
                snprintf(k, sizeof(k), "p%d", i);    L.animPath       = GetS(kv, k, "");
                snprintf(k, sizeof(k), "w%d", i);    L.weight         = GetF(kv, k, 1.f);
                snprintf(k, sizeof(k), "s%d", i);    L.speed          = GetF(kv, k, 1.f);
                snprintf(k, sizeof(k), "loop%d", i); L.looping        = GetI(kv, k, 1) != 0;
                snprintf(k, sizeof(k), "play%d", i); L.playing        = GetI(kv, k, 1) != 0;
                snprintf(k, sizeof(k), "mask%d", i); L.maskMode       = static_cast<uint8_t>(GetI(kv, k, 1));
                snprintf(k, sizeof(k), "root%d", i); L.customRootBone = GetS(kv, k, "");
            }
            w.AddComponent<AnimLayerComponent>(e, lc);
        }
    });

    // ==== AIIntentComponent (INLINE) ====
    // Strategic-layer intent: what the AI wants to achieve. Its lifetime is goal-scoped (seconds),
    // independent of path / animation churn.
    reg.Register(std::type_index(typeid(AIIntentComponent)), {
        "AIIntent",
        [](World& w, Entity e) { return w.GetComponent<AIIntentComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* i = w.GetComponent<AIIntentComponent>(e);
            if (!i) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  AIIntent: goal=%u target=%u goalPos=%.4f_%.4f_%.4f"
                " priority=%.4f\n",
                static_cast<unsigned>(i->currentGoal),
                static_cast<unsigned>(i->targetEntity),
                i->goalPosition.x, i->goalPosition.y, i->goalPosition.z,
                i->goalPriority);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            AIIntentComponent i;
            i.currentGoal  = static_cast<AIGoal>(GetI(kv, "goal", 0));
            i.targetEntity = static_cast<Entity>(GetI(kv, "target", 0));
            auto mit = kv.find("goalPos");
            if (mit != kv.end())
                sscanf_s(mit->second.c_str(), "%f_%f_%f",
                         &i.goalPosition.x, &i.goalPosition.y, &i.goalPosition.z);
            i.goalPriority = GetF(kv, "priority", 0.f);
            w.AddComponent<AIIntentComponent>(e, i);
        }
    });

    // ==== PerceptionComponent (INLINE) ====
    // Tunables only: PerceptionSystem repopulates the visibleEntities / heardSounds runtime tables
    // each tick.
    reg.Register(std::type_index(typeid(PerceptionComponent)), {
        "Perception",
        [](World& w, Entity e) { return w.GetComponent<PerceptionComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<PerceptionComponent>(e);
            if (!p) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  Perception: sightRange=%.4f sightConeDeg=%.4f"
                " hearingRange=%.4f audibleFor=%.4f"
                " eyeHeight=%.4f faction=%u threatMemory=%.4f"
                " tickInterval=%.4f maxCandidates=%u los=%u layerMask=%u\n",
                p->sightRange, p->sightConeDeg,
                p->hearingRange, p->audibleFor,
                p->eyeHeight, p->faction, p->threatMemory,
                p->tickInterval, static_cast<unsigned>(p->maxCandidates),
                p->requireLineOfSight ? 1u : 0u,
                static_cast<unsigned>(p->layerMask));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            PerceptionComponent p;
            p.sightRange   = GetF(kv, "sightRange",   15.f);
            p.sightConeDeg = GetF(kv, "sightConeDeg", 90.f);
            p.hearingRange = GetF(kv, "hearingRange", 12.f);
            p.audibleFor   = GetF(kv, "audibleFor",    8.f);
            // Absent keys keep the struct defaults, so an older scene without them loads unchanged.
            p.eyeHeight     = GetF(kv, "eyeHeight",     1.6f);
            p.faction       = static_cast<std::uint32_t>(GetI(kv, "faction", 0));
            p.threatMemory  = GetF(kv, "threatMemory",  5.f);
            p.tickInterval  = GetF(kv, "tickInterval",  0.1f);
            p.maxCandidates = static_cast<std::uint16_t>(GetI(kv, "maxCandidates", 64));
            p.requireLineOfSight = GetI(kv, "los", 1) != 0;
            p.layerMask     = static_cast<std::uint16_t>(GetI(kv, "layerMask", 0));
            w.AddComponent<PerceptionComponent>(e, p);
        }
    });

    // ==== NavAgentComponent (INLINE) ====
    // Tactical-layer intent + steering tunables. AITacticalSystem writes destination / facingMode /
    // lookTarget, persisted so a save snapshots the mid-frame combat state. Runtime fields (path[],
    // nextWaypoint, pathValid, stuckTimer) are recomputed on the first tick after load.
    reg.Register(std::type_index(typeid(NavAgentComponent)), {
        "NavAgent",
        [](World& w, Entity e) { return w.GetComponent<NavAgentComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* a = w.GetComponent<NavAgentComponent>(e);
            if (!a) return;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "  NavAgent: hasDest=%u dest=%.4f_%.4f_%.4f"
                " useLookAt=%u lookTarget=%.4f_%.4f_%.4f"
                " facingMode=%u facingTarget=%u"
                " speed=%.4f arriveRadius=%.4f slowdownRadius=%.4f"
                " repathDistance=%.4f rotateToFacing=%u turnRate=%.4f\n",
                a->hasDestination ? 1u : 0u,
                a->destination.x, a->destination.y, a->destination.z,
                a->useLookAt ? 1u : 0u,
                a->lookTarget.x, a->lookTarget.y, a->lookTarget.z,
                static_cast<unsigned>(a->facingMode),
                static_cast<unsigned>(a->facingTarget),
                a->speed, a->arriveRadius, a->slowdownRadius,
                a->repathDistance,
                a->rotateToFacing ? 1u : 0u, a->turnRate);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            NavAgentComponent a;
            a.hasDestination = GetI(kv, "hasDest", 0) != 0;
            auto dit = kv.find("dest");
            if (dit != kv.end())
                sscanf_s(dit->second.c_str(), "%f_%f_%f",
                         &a.destination.x, &a.destination.y, &a.destination.z);
            a.useLookAt = GetI(kv, "useLookAt", 0) != 0;
            auto lit = kv.find("lookTarget");
            if (lit != kv.end())
                sscanf_s(lit->second.c_str(), "%f_%f_%f",
                         &a.lookTarget.x, &a.lookTarget.y, &a.lookTarget.z);
            a.facingMode    = static_cast<NavFacingMode>(GetI(kv, "facingMode", 0));
            a.facingTarget  = static_cast<Entity>(GetI(kv, "facingTarget", 0));
            a.speed          = GetF(kv, "speed",          3.0f);
            a.arriveRadius   = GetF(kv, "arriveRadius",   0.25f);
            a.slowdownRadius = GetF(kv, "slowdownRadius", 1.5f);
            a.repathDistance = GetF(kv, "repathDistance", 1.0f);
            a.rotateToFacing = GetI(kv, "rotateToFacing", 1) != 0;
            a.turnRate       = GetF(kv, "turnRate",       8.0f);
            // An older line's hasTarget / target / groundSnap / groundSnapHeight keys are ignored: the move
            // destination is dest / hasDest on this component, and ground snap belongs to the
            // CharacterController.
            w.AddComponent<NavAgentComponent>(e, a);
        }
    });

    // ==== PerceptibleComponent (INLINE) ====
    reg.Register(std::type_index(typeid(PerceptibleComponent)), {
        "Perceptible",
        [](World& w, Entity e) { return w.GetComponent<PerceptibleComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<PerceptibleComponent>(e);
            if (!p) return;
            char buf[128];
            snprintf(buf, sizeof(buf), "  Perceptible: faction=%u offsetY=%.4f enabled=%d\n",
                     p->faction, p->offsetY,p->enabled);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            PerceptibleComponent p;
            p.enabled=GetI(kv,"enabled",1)!=0;
            p.faction = static_cast<std::uint32_t>(GetI(kv, "faction", 0));
            p.offsetY = GetF(kv, "offsetY", 1.2f);
            w.AddComponent<PerceptibleComponent>(e, p);
        }
    });

    // ==== CapsuleColliderComponent (INLINE) ====
    reg.Register(std::type_index(typeid(CapsuleColliderComponent)), {
        "CapsuleCollider",
        [](World& w, Entity e) { return w.GetComponent<CapsuleColliderComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* cc = w.GetComponent<CapsuleColliderComponent>(e);
            if (!cc || cc->count == 0) return;
            // All capsules on one line, as indexed keys (c<i>_*).
            ss << "  CapsuleCollider: count=" << cc->count;
            for (int i = 0; i < cc->count; ++i)
            {
                const auto& c = cc->capsules[i];
                char buf[256];
                snprintf(buf, sizeof(buf),
                    " c%d_boneA=%u c%d_boneB=%u c%d_radius=%.4f c%d_enabled=%d"
                    " c%d_offAx=%.4f c%d_offAy=%.4f c%d_offAz=%.4f"
                    " c%d_offBx=%.4f c%d_offBy=%.4f c%d_offBz=%.4f",
                    i, c.boneA, i, c.boneB, i, c.radius, i, c.enabled ? 1 : 0,
                    i, c.offsetA.x, i, c.offsetA.y, i, c.offsetA.z,
                    i, c.offsetB.x, i, c.offsetB.y, i, c.offsetB.z);
                ss << buf;
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            CapsuleColliderComponent cc;
            cc.count = GetI(kv, "count", 0);
            if (cc.count > CapsuleColliderComponent::MAX_CAPSULES)
                cc.count = CapsuleColliderComponent::MAX_CAPSULES;
            for (int i = 0; i < cc.count; ++i)
            {
                auto& c = cc.capsules[i];
                char key[32];
                snprintf(key, sizeof(key), "c%d_boneA", i);   c.boneA   = static_cast<uint32_t>(GetI(kv, key, 0));
                snprintf(key, sizeof(key), "c%d_boneB", i);   c.boneB   = static_cast<uint32_t>(GetI(kv, key, 0));
                snprintf(key, sizeof(key), "c%d_radius", i);  c.radius  = GetF(kv, key, 0.05f);
                snprintf(key, sizeof(key), "c%d_enabled", i); c.enabled = GetI(kv, key, 1) != 0;
                snprintf(key, sizeof(key), "c%d_offAx", i);   c.offsetA.x = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offAy", i);   c.offsetA.y = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offAz", i);   c.offsetA.z = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offBx", i);   c.offsetB.x = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offBy", i);   c.offsetB.y = GetF(kv, key, 0.f);
                snprintf(key, sizeof(key), "c%d_offBz", i);   c.offsetB.z = GetF(kv, key, 0.f);
            }
            w.AddComponent<CapsuleColliderComponent>(e, cc);
        }
    });

    // ==== ScriptComponent (INLINE, one "Script:" line per attached slot) ====
    // An entity may have several scripts: one indented "Script:" line per slot. The scene / prefab
    // dispatcher calls deserialize once per line, so each line appends a slot; a single line loads as
    // a one-slot component. An empty component writes a "slots=0" marker line so it survives a round
    // trip.
    reg.Register(std::type_index(typeid(ScriptComponent)), {
        "Script",
        [](World& w, Entity e) { return w.GetComponent<ScriptComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* sc = w.GetComponent<ScriptComponent>(e);
            if (!sc) return;
            if (sc->scripts.empty())
            {
                ss << "  Script: slots=0\n";  // marker: keep the (inert) component on reload
                return;
            }
            for (const auto& si : sc->scripts)
            {
                ss << "  Script: path=" << PercentEncode(si.scriptPath)
                   << " enabled=" << (si.enabled ? 1 : 0);
                // Exposed-variable overrides: one `var.<name>=<typechar>:<val>` token per entry (mirrors the
                // MaterialOverride inline format).
                for (const auto& [name, val] : si.vars)
                {
                    ss << " var." << PercentEncode(name) << "=" << ScriptVarTypeToChar(val.type) << ":";
                    switch (val.type)
                    {
                    case ScriptVarType::Float:  ss << val.data.f; break;
                    case ScriptVarType::Int:    ss << val.data.i; break;
                    case ScriptVarType::Bool:   ss << (val.data.b ? 1 : 0); break;
                    case ScriptVarType::Float3:
                    case ScriptVarType::Color:  ss << val.data.v3[0] << "_" << val.data.v3[1]
                                                   << "_" << val.data.v3[2]; break;
                    case ScriptVarType::Entity:
                    {
                        // Persist by GUID (stable across save/load + prefab
                        // instantiation), mirroring WriteAttachmentRef. Prefer the
                        // stored GUID; otherwise stamp one on the live target.
                        ECS::Guid g = ECS::kInvalidGuid;
                        if (!val.str.empty())          g = ECS::Guid::FromHex(val.str);
                        else if (val.data.entity != 0) g = ECS::EnsureGuidOn(w, (Entity)val.data.entity);
                        ss << (g.IsValid() ? g.ToHex() : std::string("0"));
                    } break;
                    case ScriptVarType::String:
                    case ScriptVarType::Asset:  ss << PercentEncode(val.str); break;
                    }
                }
                ss << "\n";
            }
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            // Get-or-create the component, then append this line's slot. Calling
            // AddComponent fresh would clobber slots appended by earlier lines.
            ScriptComponent* sc = w.GetComponent<ScriptComponent>(e);
            if (!sc) { w.AddComponent<ScriptComponent>(e, {}); sc = w.GetComponent<ScriptComponent>(e); }
            if (!sc) return;

            // Marker line of an empty component (no "path" key) -> no slot.
            auto pathIt = kv.find("path");
            if (pathIt == kv.end()) return;

            ScriptInstance si;
            si.scriptPath = PercentDecode(pathIt->second);
            si.enabled    = GetI(kv, "enabled", 1) != 0;
            // Exposed-variable override tokens (`var.<name>=<typechar>:<val>`); none -> an empty bag (the
            // script defaults apply).
            for (const auto& [rawKey, rawVal] : kv)
            {
                if (rawKey.rfind("var.", 0) != 0) continue;
                if (rawVal.size() < 2 || rawVal[1] != ':') continue;
                ScriptVarType type;
                if (!ScriptVarTypeFromChar(rawVal[0], type)) continue;
                const std::string name = PercentDecode(rawKey.substr(4));
                const std::string vs   = rawVal.substr(2);
                try {
                    switch (type)
                    {
                    case ScriptVarType::Float:  si.vars[name] = ScriptVarValue::MakeFloat(std::stof(vs)); break;
                    case ScriptVarType::Int:    si.vars[name] = ScriptVarValue::MakeInt(std::stoi(vs));   break;
                    case ScriptVarType::Bool:   si.vars[name] = ScriptVarValue::MakeBool(vs != "0");      break;
                    case ScriptVarType::Float3:
                    case ScriptVarType::Color:
                    {
                        float x = 0.f, y = 0.f, z = 0.f;
                        sscanf_s(vs.c_str(), "%f_%f_%f", &x, &y, &z);
                        si.vars[name] = (type == ScriptVarType::Color)
                            ? ScriptVarValue::MakeColor (x, y, z)
                            : ScriptVarValue::MakeFloat3(x, y, z);
                    } break;
                    case ScriptVarType::Entity:
                    {
                        // 32-char GUID hex (resolved to an id at inject time), or an older raw decimal entity id.
                        if (vs.size() == 32 &&
                            vs.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
                            si.vars[name] = ScriptVarValue::MakeEntity(0u, vs);
                        else
                            si.vars[name] = ScriptVarValue::MakeEntity((uint32_t)std::stoul(vs));
                    } break;
                    case ScriptVarType::String: si.vars[name] = ScriptVarValue::MakeString(PercentDecode(vs)); break;
                    case ScriptVarType::Asset:  si.vars[name] = ScriptVarValue::MakeAsset (PercentDecode(vs)); break;
                    }
                } catch (...) {}
            }
            sc->scripts.push_back(std::move(si));
        }
    });

    // ==== SocketComponent (INLINE, per-entity attachment points) ====
    //   Sockets: count=N n0=<percent-encoded-name> b0=<boneIdx> bn0=<bone name> m0=<f0_..._f15>
    //                    n1=...                   b1=...        bn1=...          m1=...
    // worldTransform is not serialized: SocketSystem rewrites it every frame from the bone pose. Only
    // the entity's own sockets are written; the ones derived from the model's SO records come back
    // from the model at spawn. bn<i> names the bone (an index alone would follow whatever bone a
    // re-import put there): SocketSystem resolves by it, b<i> is the hint (and all an older record
    // has). A load merges: the entity's own list is replaced, the derived ones stay, and the
    // generation moves on (a reset to 1 could equal a follower's cached generation and keep it on a
    // stale index).
    reg.Register(std::type_index(typeid(SocketComponent)), {
        "Sockets",
        [](World& w, Entity e) {
            const auto* sc = w.GetComponent<SocketComponent>(e);
            return sc && sc->AuthoredCount() > 0;
        },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* sc = w.GetComponent<SocketComponent>(e);
            if (!sc || sc->AuthoredCount() == 0) return;
            ss << "  Sockets: count=" << sc->AuthoredCount();
            char buf[32];
            uint32_t i = 0;
            for (uint32_t j = 0; j < sc->count; ++j)
            {
                const auto& s = sc->sockets[j];
                if (s.flags & SocketComponent::kDerived) continue;
                ss << " n" << i << "=" << PercentEncode(s.name);
                ss << " b" << i << "=" << s.boneIndex;
                if (s.boneName[0])
                    ss << " bn" << i << "=" << PercentEncodeText(std::string(s.boneName, strnlen(s.boneName, sizeof(s.boneName))));
                ss << " m" << i << "=";
                const float* p = reinterpret_cast<const float*>(&s.localOffset);
                for (int k = 0; k < 16; ++k)
                {
                    if (k > 0) ss << "_";
                    snprintf(buf, sizeof(buf), "%.6f", p[k]);
                    ss << buf;
                }
                ++i;
            }
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            // 32 sockets x ~280 B: on the heap, not the serializer's stack.
            auto list = std::make_unique<SocketComponent::Socket[]>(SocketComponent::MAX);
            int count = GetI(kv, "count", 0);
            if (count < 0) count = 0;
            if (count > static_cast<int>(SocketComponent::MAX))
                count = static_cast<int>(SocketComponent::MAX);
            const uint32_t n = static_cast<uint32_t>(count);

            for (uint32_t i = 0; i < n; ++i)
            {
                const std::string nKey  = "n"  + std::to_string(i);
                const std::string bKey  = "b"  + std::to_string(i);
                const std::string bnKey = "bn" + std::to_string(i);
                const std::string mKey  = "m"  + std::to_string(i);

                auto& s = list[i];
                const std::string name = GetS(kv, nKey.c_str(), "");
                const size_t copy = std::min(name.size(), sizeof(s.name) - 1);
                std::memcpy(s.name, name.c_str(), copy);
                s.name[copy] = '\0';
                s.boneIndex = static_cast<uint32_t>(GetI(kv, bKey.c_str(), 0));
                // GetS decodes: a second PercentDecode would read a bone named "Bone%41" back as "BoneA".
                const std::string bone = GetS(kv, bnKey.c_str(), "");
                const size_t bcopy = std::min(bone.size(), sizeof(s.boneName) - 1);
                std::memcpy(s.boneName, bone.c_str(), bcopy);
                s.boneName[bcopy] = '\0';

                DirectX::XMStoreFloat4x4(&s.localOffset, DirectX::XMMatrixIdentity());
                DirectX::XMStoreFloat4x4(&s.worldTransform, DirectX::XMMatrixIdentity());

                const std::string mStr = GetS(kv, mKey.c_str(), "");
                if (!mStr.empty())
                {
                    float* p = reinterpret_cast<float*>(&s.localOffset);
                    size_t start = 0;
                    for (int k = 0; k < 16; ++k)
                    {
                        size_t end = mStr.find('_', start);
                        if (end == std::string::npos) end = mStr.size();
                        try { p[k] = std::stof(mStr.substr(start, end - start)); }
                        catch (...) {}
                        if (end == mStr.size()) break;
                        start = end + 1;
                    }

                    // Seed the Euler authoring repr from the loaded matrix with the decomposition the Inspector
                    // rebuilds the rotation from (XMQuaternionRotationRollPitchYaw), so it starts on a stable
                    // representation; a ZYX decomposition would turn a mixed-axis socket the first time only its
                    // Offset was edited.
                    using namespace DirectX;
                    XMVECTOR vScl, vRot, vTrn;
                    if (XMMatrixDecompose(&vScl, &vRot, &vTrn,
                                          XMLoadFloat4x4(&s.localOffset)))
                    {
                        XMFLOAT4 q;
                        XMStoreFloat4(&q, vRot);
                        s.rotationEulerDeg = MathUtils::RollPitchYawDegreesFromQuaternion(q);
                    }
                }
            }

            if (auto* existing = w.GetComponent<SocketComponent>(e))
                existing->ReplaceAuthored(list.get(), n);
            else
            {
                auto sc = std::make_unique<SocketComponent>();
                sc->ReplaceAuthored(list.get(), n);
                w.AddComponent<SocketComponent>(e, *sc);
            }
        }
    });

    // ---- UI components ----
    // Canvas layout output and world-UI GPU bindings are runtime-only; the authored Canvas and
    // world-space components below round-trip through scenes and prefabs.

    // ==== Entity-as-widget Canvas UI (UI/UICanvas.h) ====
    RegisterReflectedSerializer<UI::UICanvas>(reg, "CvCanvas", Reflect::NumberStyle::Stream, ComponentFlag::LuaWritable);

    RegisterReflectedSerializer<UI::UIRect>(reg, "CvRect", Reflect::NumberStyle::Stream, ComponentFlag::LuaWritable);

    // CvDoc: a .iui UI asset the scene references. Only the path belongs to the scene; the instance
    // (UI::UpdateDocuments) is runtime, every node SceneTransientTag, rebuilt from the file at each
    // load.
    RegisterReflectedSerializer<UI::UIDocument>(reg, "CvDoc", Reflect::NumberStyle::Stream, ComponentFlag::LuaWritable);

    RegisterReflectedSerializer<UI::UIClipRect>(reg, "CvClip", Reflect::NumberStyle::Stream, ComponentFlag::LuaWritable);

    reg.Register(std::type_index(typeid(UI::UIParent)), {
        "CvParent",
        [](World& w, Entity e) { return w.GetComponent<UI::UIParent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* p = w.GetComponent<UI::UIParent>(e);
            if (!p) return;
            // Stamp the parent's GUID (idempotent) so the edge round-trips.
            ECS::Guid g = p->parentGuid;
            if (p->parent != NullEntity && w.IsAlive(p->parent))
                g = ECS::EnsureGuidOn(w, p->parent);
            if (g.IsValid())
                ss << "  CvParent: parentGuid=" << g.ToHex() << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UIParent p;
            const std::string gh = GetS(kv, "parentGuid");
            if (!gh.empty()) p.parentGuid = ECS::Guid::FromHex(gh);
            // parent entity resolved from parentGuid by UICanvasSystem at tick.
            w.AddComponent<UI::UIParent>(e, p);
        }
    });

    reg.Register(std::type_index(typeid(UI::UIImage)), {
        "CvImage",
        [](World& w, Entity e) { return w.GetComponent<UI::UIImage>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* i = w.GetComponent<UI::UIImage>(e);
            if (!i) return;
            ss << "  CvImage: visible=" << (i->visible ? 1u : 0u)
               << " color=" << i->color.x << "_" << i->color.y << "_" << i->color.z << "_" << i->color.w
               << " uv0=" << i->uv0.x << "_" << i->uv0.y
               << " uv1=" << i->uv1.x << "_" << i->uv1.y
               << " wrap=" << static_cast<uint32_t>(i->wrapMode)
               << " point=" << (i->pointFilter ? 1u : 0u)
               << " slice=" << i->sliceBorder.x << "_" << i->sliceBorder.y << "_"
                            << i->sliceBorder.z << "_" << i->sliceBorder.w
               << " tex=" << PercentEncode(i->texturePath) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UIImage i;
            i.visible = GetI(kv, "visible", 1) != 0;
            const std::string cs = GetS(kv, "color");
            if (!cs.empty()) sscanf_s(cs.c_str(), "%f_%f_%f_%f", &i.color.x, &i.color.y, &i.color.z, &i.color.w);
            const std::string u0 = GetS(kv, "uv0");
            if (!u0.empty()) sscanf_s(u0.c_str(), "%f_%f", &i.uv0.x, &i.uv0.y);
            const std::string u1 = GetS(kv, "uv1");
            if (!u1.empty()) sscanf_s(u1.c_str(), "%f_%f", &i.uv1.x, &i.uv1.y);
            i.wrapMode    = static_cast<UI::UIWrapMode>(GetI(kv, "wrap", 0));
            i.pointFilter = GetI(kv, "point", 0) != 0;
            // Absent = all zeros = a single stretched quad (no 9-slice).
            const std::string sb = GetS(kv, "slice");
            if (!sb.empty())
                sscanf_s(sb.c_str(), "%f_%f_%f_%f", &i.sliceBorder.x, &i.sliceBorder.y,
                                                    &i.sliceBorder.z, &i.sliceBorder.w);
            i.texturePath = GetS(kv, "tex");
            w.AddComponent<UI::UIImage>(e, i);
        }
    });

    reg.Register(std::type_index(typeid(UI::UISpriteAnimComponent)), {
        "CvSpriteAnim",
        [](World& w, Entity e) { return w.GetComponent<UI::UISpriteAnimComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* s = w.GetComponent<UI::UISpriteAnimComponent>(e);
            if (!s) return;
            ss << "  CvSpriteAnim: cols=" << s->columns << " rows=" << s->rows
               << " frames=" << s->frameCount << " fps=" << s->fps
               << " loop=" << (s->loop ? 1u : 0u)
               << " playing=" << (s->playing ? 1u : 0u)
               << " pingpong=" << (s->pingpong ? 1u : 0u) << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UISpriteAnimComponent s;
            s.columns    = GetI(kv, "cols", 4);
            s.rows       = GetI(kv, "rows", 4);
            s.frameCount = GetI(kv, "frames", 0);
            s.fps        = GetF(kv, "fps", 12.f);
            s.loop       = GetI(kv, "loop", 1) != 0;
            s.playing    = GetI(kv, "playing", 1) != 0;
            s.pingpong   = GetI(kv, "pingpong", 0) != 0;
            w.AddComponent<UI::UISpriteAnimComponent>(e, s);
        }
    });

    reg.Register(std::type_index(typeid(UI::UIText)), {
        "CvText",
        [](World& w, Entity e) { return w.GetComponent<UI::UIText>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* t = w.GetComponent<UI::UIText>(e);
            if (!t) return;
            // PercentEncodeText: a newline (the UI Editor's Ctrl+Enter) or a tab would end the line or the
            // token, losing the rest of the text on save and in every undo blob. Same bytes otherwise.
            ss << "  CvText: visible=" << (t->visible ? 1u : 0u)
               << " text=" << PercentEncodeText(t->text)
               << " font=" << PercentEncode(t->fontPath)
               << " color=" << t->color.x << "_" << t->color.y << "_" << t->color.z << "_" << t->color.w
               << " fontScale=" << t->fontScale
               << " alignH=" << t->alignH << " alignV=" << t->alignV
               << " fxO=" << (t->fxOutline ? 1u : 0u)
               << " fxG=" << (t->fxGlow    ? 1u : 0u)
               << " fxS=" << (t->fxShadow  ? 1u : 0u)
               << " fxJ=" << (t->fxJitter  ? 1u : 0u)
               << " outlineCol=" << t->outlineColor.x << "_" << t->outlineColor.y << "_"
                                 << t->outlineColor.z << "_" << t->outlineColor.w
               << " outlineW=" << t->outlineWidth
               << " glowCol=" << t->glowColor.x << "_" << t->glowColor.y << "_"
                              << t->glowColor.z << "_" << t->glowColor.w
               << " glowW=" << t->glowWidth
               << " shadowCol=" << t->shadowColor.x << "_" << t->shadowColor.y << "_"
                                << t->shadowColor.z << "_" << t->shadowColor.w
               << " shadowOX=" << t->shadowOffsetX << " shadowOY=" << t->shadowOffsetY
               << " jitterA=" << t->jitterAmplitude << " jitterF=" << t->jitterFrequency
               << " customShader=" << PercentEncode(t->customShaderPath)
               << " cpA=" << t->customParam0.x << "_" << t->customParam0.y << "_"
                          << t->customParam0.z << "_" << t->customParam0.w
               << " cpB=" << t->customParam1.x << "_" << t->customParam1.y << "_"
                          << t->customParam1.z << "_" << t->customParam1.w
               << " wrapW=" << t->wrapWidth << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::UIText t;
            t.text     = GetS(kv, "text");
            t.fontPath = GetS(kv, "font"); // absent in old scenes -> default font
            auto parseV4 = [&](const char* k, DirectX::XMFLOAT4& out) {
                const std::string s = GetS(kv, k);
                if (!s.empty()) sscanf_s(s.c_str(), "%f_%f_%f_%f", &out.x, &out.y, &out.z, &out.w);
            };
            parseV4("color", t.color);
            t.visible   = GetI(kv, "visible", 1) != 0;
            t.fontScale = GetF(kv, "fontScale", 1.f);
            t.alignH    = GetI(kv, "alignH", 1);
            t.alignV    = GetI(kv, "alignV", 1);
            // Per-effect toggles; an older line carries a preset int ("effect=") instead, mapped as 1 Shadow,
            // 2 Outline, 3 Glow, 4 Jitter, 5 Outline+Shadow, 6 Glow+Shadow.
            const int legacy = GetI(kv, "effect", 0);
            t.fxOutline = GetI(kv, "fxO", (legacy == 2 || legacy == 5) ? 1 : 0) != 0;
            t.fxGlow    = GetI(kv, "fxG", (legacy == 3 || legacy == 6) ? 1 : 0) != 0;
            t.fxShadow  = GetI(kv, "fxS", (legacy == 1 || legacy == 5 || legacy == 6) ? 1 : 0) != 0;
            t.fxJitter  = GetI(kv, "fxJ", (legacy == 4) ? 1 : 0) != 0;
            parseV4("outlineCol", t.outlineColor); t.outlineWidth = GetF(kv, "outlineW", 2.f);
            parseV4("glowCol", t.glowColor);       t.glowWidth    = GetF(kv, "glowW", 4.f);
            parseV4("shadowCol", t.shadowColor);
            t.shadowOffsetX   = GetF(kv, "shadowOX", 2.f);
            t.shadowOffsetY   = GetF(kv, "shadowOY", 2.f);
            t.jitterAmplitude = GetF(kv, "jitterA", 2.f);
            t.jitterFrequency = GetF(kv, "jitterF", 12.f);
            t.customShaderPath = GetS(kv, "customShader");
            parseV4("cpA", t.customParam0);
            parseV4("cpB", t.customParam1);
            // Absent -> 0 = no wrapping, so an older scene's text does not start wrapping.
            t.wrapWidth = GetF(kv, "wrapW", 0.f);
            w.AddComponent<UI::UIText>(e, t);
        }
    });

    RegisterReflectedSerializer<UI::UIInteractable>(reg, "CvInteract", Reflect::NumberStyle::Stream, ComponentFlag::LuaWritable);

    // ==== UI::WorldSpaceUIComponent (INLINE, computed fields skipped) ====
    reg.Register(std::type_index(typeid(UI::WorldSpaceUIComponent)), {
        "WorldUI",
        [](World& w, Entity e) { return w.GetComponent<UI::WorldSpaceUIComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* u = w.GetComponent<UI::WorldSpaceUIComponent>(e);
            if (!u) return;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "  WorldUI: baseW=%.4f baseH=%.4f pivotX=%.4f pivotY=%.4f"
                " offX=%.4f offY=%.4f offZ=%.4f scalingMode=%u"
                " fadeNear=%.4f fadeFar=%.4f hideBehind=%u depthMode=%u\n",
                u->baseSize.x, u->baseSize.y, u->pivot.x, u->pivot.y,
                u->screenSpaceOffset.x, u->screenSpaceOffset.y, u->screenSpaceOffset.z,
                static_cast<uint32_t>(u->scalingMode),
                u->fadeNear, u->fadeFar,
                u->hideWhenBehindCamera ? 1u : 0u,
                static_cast<uint32_t>(u->depthMode));
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::WorldSpaceUIComponent u;
            u.baseSize          = { GetF(kv, "baseW", 1.f),  GetF(kv, "baseH", 0.2f) };
            u.pivot             = { GetF(kv, "pivotX", 0.5f), GetF(kv, "pivotY", 1.f) };
            u.screenSpaceOffset = { GetF(kv, "offX", 0.f), GetF(kv, "offY", 0.f), GetF(kv, "offZ", 0.f) };
            u.scalingMode       = static_cast<UI::ScalingMode>(GetI(kv, "scalingMode", (int)UI::ScalingMode::ConstantWorld));
            u.fadeNear          = GetF(kv, "fadeNear", 0.f);
            u.fadeFar           = GetF(kv, "fadeFar", 50.f);
            u.hideWhenBehindCamera = GetI(kv, "hideBehind", 1) != 0;
            u.depthMode         = static_cast<UI::DepthMode>(GetI(kv, "depthMode", (int)UI::DepthMode::Always));
            w.AddComponent<UI::WorldSpaceUIComponent>(e, u);
        }
    });

    // ==== UI::WorldUIBarComponent (INLINE) ====
    reg.Register(std::type_index(typeid(UI::WorldUIBarComponent)), {
        "WorldUIBar",
        [](World& w, Entity e) { return w.GetComponent<UI::WorldUIBarComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* b = w.GetComponent<UI::WorldUIBarComponent>(e);
            if (!b) return;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "  WorldUIBar: value=%.4f borderThick=%.4f"
                " fill=%.4f_%.4f_%.4f_%.4f bg=%.4f_%.4f_%.4f_%.4f"
                " border=%.4f_%.4f_%.4f_%.4f visible=%u\n",
                b->value, b->borderThick,
                b->fillColor.x, b->fillColor.y, b->fillColor.z, b->fillColor.w,
                b->backgroundColor.x, b->backgroundColor.y, b->backgroundColor.z, b->backgroundColor.w,
                b->borderColor.x, b->borderColor.y, b->borderColor.z, b->borderColor.w,
                b->visible ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::WorldUIBarComponent b;
            b.value       = GetF(kv, "value", 1.f);
            b.borderThick = GetF(kv, "borderThick", 0.02f);
            auto parseV4 = [&](const char* k, DirectX::XMFLOAT4& out) {
                const std::string s = GetS(kv, k);
                if (!s.empty()) sscanf_s(s.c_str(), "%f_%f_%f_%f", &out.x, &out.y, &out.z, &out.w);
            };
            parseV4("fill",   b.fillColor);
            parseV4("bg",     b.backgroundColor);
            parseV4("border", b.borderColor);
            b.visible = GetI(kv, "visible", 1) != 0;
            w.AddComponent<UI::WorldUIBarComponent>(e, b);
        }
    });

    // ==== UI::WorldUITextComponent (INLINE) ====
    reg.Register(std::type_index(typeid(UI::WorldUITextComponent)), {
        "WorldUIText",
        [](World& w, Entity e) { return w.GetComponent<UI::WorldUITextComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* t = w.GetComponent<UI::WorldUITextComponent>(e);
            if (!t) return;
            auto v4 = [&ss](const char* k, const DirectX::XMFLOAT4& c) {
                ss << " " << k << "=" << c.x << "_" << c.y << "_" << c.z << "_" << c.w;
            };
            ss << "  WorldUIText: text=" << PercentEncodeText(t->text)   // a newline must not end the line
               << " font=" << PercentEncode(t->fontPath)
               << " color=" << t->color.x << "_" << t->color.y << "_"
               << t->color.z << "_" << t->color.w
               << " scale=" << t->scale
               << " visible=" << (t->visible ? 1u : 0u)
               << " fxO=" << (t->fxOutline ? 1u : 0u)
               << " fxG=" << (t->fxGlow    ? 1u : 0u)
               << " fxS=" << (t->fxShadow  ? 1u : 0u)
               << " fxJ=" << (t->fxJitter  ? 1u : 0u);
            v4("outlineCol", t->outlineColor);
            ss << " outlineW=" << t->outlineWidth;
            v4("glowCol", t->glowColor);
            ss << " glowW=" << t->glowWidth;
            v4("shadowCol", t->shadowColor);
            ss << " shadowOX=" << t->shadowOffsetX << " shadowOY=" << t->shadowOffsetY
               << " jitterA=" << t->jitterAmplitude << " jitterF=" << t->jitterFrequency
               << " customShader=" << PercentEncode(t->customShaderPath);
            v4("cpA", t->customParam0);
            v4("cpB", t->customParam1);
            ss << "\n";
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::WorldUITextComponent t;
            t.text     = GetS(kv, "text");
            t.fontPath = GetS(kv, "font"); // absent in old scenes -> default font
            auto parseV4 = [&](const char* k, DirectX::XMFLOAT4& out) {
                const std::string s = GetS(kv, k);
                if (!s.empty()) sscanf_s(s.c_str(), "%f_%f_%f_%f", &out.x, &out.y, &out.z, &out.w);
            };
            parseV4("color", t.color);
            t.scale   = GetF(kv, "scale", 1.f);
            t.visible = GetI(kv, "visible", 1) != 0;
            t.fxOutline = GetI(kv, "fxO", 0) != 0;
            t.fxGlow    = GetI(kv, "fxG", 0) != 0;
            t.fxShadow  = GetI(kv, "fxS", 0) != 0;
            t.fxJitter  = GetI(kv, "fxJ", 0) != 0;
            parseV4("outlineCol", t.outlineColor); t.outlineWidth = GetF(kv, "outlineW", 2.f);
            parseV4("glowCol", t.glowColor);       t.glowWidth    = GetF(kv, "glowW", 4.f);
            parseV4("shadowCol", t.shadowColor);
            t.shadowOffsetX   = GetF(kv, "shadowOX", 2.f);
            t.shadowOffsetY   = GetF(kv, "shadowOY", 2.f);
            t.jitterAmplitude = GetF(kv, "jitterA", 2.f);
            t.jitterFrequency = GetF(kv, "jitterF", 12.f);
            t.customShaderPath = GetS(kv, "customShader");
            parseV4("cpA", t.customParam0);
            parseV4("cpB", t.customParam1);
            w.AddComponent<UI::WorldUITextComponent>(e, t);
        }
    });

    // ==== UI::WorldUIImageComponent (INLINE, bindlessIndex is runtime) ====
    reg.Register(std::type_index(typeid(UI::WorldUIImageComponent)), {
        "WorldUIImage",
        [](World& w, Entity e) { return w.GetComponent<UI::WorldUIImageComponent>(e) != nullptr; },
        [](World& w, Entity e, std::ostringstream& ss) {
            const auto* i = w.GetComponent<UI::WorldUIImageComponent>(e);
            if (!i) return;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "  WorldUIImage: tint=%.4f_%.4f_%.4f_%.4f"
                " uv0X=%.4f uv0Y=%.4f uv1X=%.4f uv1Y=%.4f visible=%u\n",
                i->tint.x, i->tint.y, i->tint.z, i->tint.w,
                i->uv0.x, i->uv0.y, i->uv1.x, i->uv1.y,
                i->visible ? 1u : 0u);
            ss << buf;
        },
        [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
            UI::WorldUIImageComponent i;
            const std::string ts = GetS(kv, "tint");
            if (!ts.empty())
                sscanf_s(ts.c_str(), "%f_%f_%f_%f",
                    &i.tint.x, &i.tint.y, &i.tint.z, &i.tint.w);
            i.uv0 = { GetF(kv, "uv0X", 0.f), GetF(kv, "uv0Y", 0.f) };
            i.uv1 = { GetF(kv, "uv1X", 1.f), GetF(kv, "uv1Y", 1.f) };
            i.visible = GetI(kv, "visible", 1) != 0;
            // bindlessIndex stays ~0u: TextureSystem repopulates it after spawn.
            w.AddComponent<UI::WorldUIImageComponent>(e, i);
        }
    });
}

namespace
{
    // The loaders' own records (resolved by the scene / prefab loader), never a component.
    bool IsLoaderRecord(const std::string& tag)
    {
        return tag == "SceneRef" || tag == "AnimRef" || tag == "PrefabRef" || tag == "PrefabFollowSocket" ||
               tag == "PrefabFollowEntity" || tag == "PrefabSkeletonRef";
    }

    // Once per tag per process.
    bool FirstTime(const std::string& tag)
    {
        static std::mutex s_lock;
        static std::unordered_set<std::string> s_seen;
        std::lock_guard<std::mutex> lock(s_lock);
        return s_seen.insert(tag).second;
    }
}

void ApplyComponentLine(const ComponentSerializerRegistry& reg, World& world, Entity e, const std::string& tag,
                        const KVMap& kv, Resource::AssetManager* assets)
{
    const Module::Registry& modules = Module::Registry::Get();
    if (const ComponentSerializer* ser = reg.FindByTag(tag))
    {
        ser->deserialize(world, e, kv, assets);
        const std::string* owner = modules.OwnerOfComponentTag(tag);
        if (owner && !modules.IsEnabled(*owner) && FirstTime(tag))
            LOG_WARNING("[MODULES] component '%s' loaded, but its module %s is off in this project: nothing runs it",
                        tag.c_str(), owner->c_str());
        return;
    }
    if (IsLoaderRecord(tag)) return;
    if (FirstTime(tag))
    {
        const std::string* owner = modules.OwnerOfComponentTag(tag);
        LOG_WARNING("[MODULES] component '%s' is kept unread: %s", tag.c_str(),
                    owner ? ("its module " + *owner + " is not in this build").c_str()
                          : "no serializer in this build reads it");
    }
    // Keys sorted: the line is written back in a stable form; the values are the raw tokens.
    std::vector<const KVMap::value_type*> pairs;
    pairs.reserve(kv.size());
    for (const auto& p : kv) pairs.push_back(&p);
    std::sort(pairs.begin(), pairs.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
    std::string line = "  " + tag + ":";
    for (const auto* p : pairs) line += " " + p->first + "=" + p->second;
    if (auto* u = world.GetComponent<UnresolvedComponents>(e))
        u->lines.push_back(std::move(line));
    else
        world.AddComponent<UnresolvedComponents>(e, UnresolvedComponents{ { std::move(line) } });
}

ComponentSerializerRegistry& GetComponentRegistry()
{
    static ComponentSerializerRegistry s_reg;
    static bool s_init = false;
    if (!s_init)
    {
        RegisterAllComponentSerializers(s_reg);
        RegisterGameplayComponentSerializers(s_reg);
        ApplyHandWrittenComponentFlags(s_reg);
        // The linked modules' serializers: each module registers its own (Module/EngineModule.h,
        // RegisterTypes).
        Module::Registry::Get().RegisterTypes(s_reg);
        // The core components' field descriptors, for every exe's tools (the catalog dump), each
        // findable by its serializer tag.
        Reflect::RegisterCoreDescriptors();
        for (const auto& [type, serializer] : s_reg.All())
            Reflect::SetRuntimeTag(type, serializer.tag.c_str());
        ValidateComponentRegistries(s_reg);
        s_init = true;
    }
    return s_reg;
}
