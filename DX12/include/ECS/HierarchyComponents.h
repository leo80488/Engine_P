#pragma once

// SceneGraph -> ECS hierarchy components: written by the scene loaders, propagated by
// TransformSystem. Pure data: keep GPU / DX12 types out of this header.

#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"      // Resource::MeshHandle
#include "Resource/MaterialSerializer.h" // Resource::MaterialLoadInfo

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <vector>
#include <unordered_set>

// LocalTransform: TRS relative to the parent node. Quaternion rotation (xyzw) for singularity-free
// composition. Written by user code / animation; TransformSystem reads it.
struct LocalTransform
{
    DirectX::XMFLOAT3 translation = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4 rotation    = { 0.f, 0.f, 0.f, 1.f };  // quaternion xyzw
    DirectX::XMFLOAT3 scale       = { 1.f, 1.f, 1.f };

    // Local SRT matrix in DirectXMath (row-vector) convention. The quaternion is normalized
    // defensively: XMMatrixRotationQuaternion is undefined on non-unit input, and worlds saved with a
    // sign-flipped / negative-zero qw can sit slightly off unit length, leaking a tiny shear-scale.
    DirectX::XMMATRIX ToMatrix() const
    {
        using namespace DirectX;
        const XMMATRIX S = XMMatrixScalingFromVector(XMLoadFloat3(&scale));
        const XMMATRIX R = XMMatrixRotationQuaternion(
            XMQuaternionNormalize(XMLoadFloat4(&rotation)));
        const XMMATRIX T = XMMatrixTranslationFromVector(XMLoadFloat3(&translation));
        return S * R * T;
    }
};

// GlobalTransform: world-space 4x4 matrix computed by TransformSystem. Do not write it by hand:
// every Propagate() overwrites it.
struct GlobalTransform
{
    DirectX::XMFLOAT4X4 matrix;
    GlobalTransform() { DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixIdentity()); }
};

// Parent: the parent entity. NullEntity = root (no parent).
struct Parent
{
    Entity entity = NullEntity;
};

// Children: ordered list of immediate child entities. Written by the scene loaders; update it
// together with Parent on reparent / destroy.
struct Children
{
    std::vector<Entity> entities;
};

// ViewBit: per-view identifier used by VisibilityComponent::viewMask. Each rendering view (main
// camera, every shadow cascade, every probe, ...) owns one bit; culling ANDs the entity's mask with
// the view's bit before any frustum / occlusion work.
namespace ViewBit
{
    constexpr uint32_t MainCamera       = 1u << 0;
    constexpr uint32_t ShadowCascade0   = 1u << 1;
    constexpr uint32_t ShadowCascade1   = 1u << 2;
    constexpr uint32_t ShadowCascade2   = 1u << 3;
    constexpr uint32_t ShadowCascade3   = 1u << 4;
    constexpr uint32_t ReflectionProbe  = 1u << 5;
    // Bit 6 is the secondary view (the in-game PiP and the editor's camera preview);
    // Preview::kSecondaryViewBit aliases it and the inspector labels it "Secondary View".
    //
    // The serialized form is the raw decimal mask (ComponentSerializers writes "Visibility:
    // viewMask=%u ..." into .iscene / .ipfb), so a name may change but a bit may not.
    // VisibilityComponent defaults to ViewBit::All (bit 6 set). The migration
    //     python tools/asset_patch.py migrate-viewbit6 asset engine [--apply]
    // ORs bit 6 into every Visibility record with MainCamera set and bit 6 clear, through the
    // container writer. It covers the .iscene / .ipfb containers under DX12/asset/ and DX12/engine/ (a
    // .iscn has no Visibility record, and nothing else loads as a world container). tools/ci_check.py
    // runs it as a dry run on every invocation (gate_viewbit6, --skip-build included) and fails with
    // exit 3 naming the --apply command; run --apply by hand after importing older content.
    // If you claim a new bit here, scan the assets: once on disk, a cleared bit is indistinguishable
    // from an author's choice.
    constexpr uint32_t SecondaryView    = 1u << 6;
    // Bits 7 and above name no view: no pass reads them. They stay set in a saved mask (All is ~0u),
    // so a bit may be claimed later only after scanning the assets, as above.

    constexpr uint32_t ShadowAny =
        ShadowCascade0 | ShadowCascade1 | ShadowCascade2 | ShadowCascade3;
    constexpr uint32_t All = ~0u;
}

// VisibilityComponent: author-intent visibility of a render entity.
//   `flags`           - user toggles (Visible / CastShadow / RenderInMainPass).
//   `viewMask`        - the views this entity takes part in (ViewBit).
//   `renderLayer`     - reserved for camera-layer filtering (1 byte; serialized, but no pass
//                       filters on it).
//   `inheritedHidden` - runtime only: TransformSystem sets it when an ancestor is invisible.
//                       Not persisted.
// Pipeline order (docs/design/ecs_visibility_design.md): VisibilityComponent filter -> BVH frustum
// cull -> HZB occlusion -> draw. Hidden entities must be dropped before the BVH gather.
struct VisibilityComponent
{
    enum Flags : uint8_t
    {
        Visible          = 1 << 0,   // user toggle: entity is shown at all
        CastShadow       = 1 << 1,   // contribute to shadow passes
        RenderInMainPass = 1 << 2,   // emit a draw in the main / GBuffer pass
    };

    uint32_t viewMask        = ViewBit::All;
    uint8_t  flags           = Visible | CastShadow | RenderInMainPass;
    uint8_t  renderLayer     = 0;
    bool     inheritedHidden = false;   // recomputed every frame by TransformSystem

    bool IsVisible() const            { return (flags & Visible) != 0; }
    bool CastsShadow() const          { return (flags & CastShadow) != 0; }
    bool RendersInMainPass() const    { return (flags & RenderInMainPass) != 0; }
    bool IsInView(uint32_t viewBit) const { return (viewMask & viewBit) != 0; }

    // Author-visible AND hierarchy-propagated: the predicate culling reads.
    bool IsEffectivelyVisible() const { return IsVisible() && !inheritedHidden; }

    void SetVisible(bool v)          { v ? flags |= Visible          : flags &= ~Visible; }
    void SetCastShadow(bool v)       { v ? flags |= CastShadow       : flags &= ~CastShadow; }
    void SetRenderInMainPass(bool v) { v ? flags |= RenderInMainPass : flags &= ~RenderInMainPass; }
};

// Structural-hide tags. Presence excludes an entity from culling without touching its
// VisibilityComponent (query the pool, branch out).
//   EditorOnlyTag   - gizmos, debug volumes; never visible in the Game build.
//   HiddenInGameTag - editor-time props that vanish at play.
//   DisabledTag     - entity logically paused: culled in all builds.
// Tags persist across save/load, so toggle them rarely; for frequent hide/show use
// VisibilityComponent::SetVisible.
struct EditorOnlyTag {};
struct HiddenInGameTag {};
struct DisabledTag {};

// Marks a system-owned helper entity that is live ECS state, not authored
// scene data. SceneSerializer omits the entire entity row (not just this tag),
// so it cannot come back as an empty entity after a scene round-trip.
//
// Do not use this to suppress one component on an authored entity: attaching
// the tag makes the whole entity non-persistent.
struct SceneTransientTag {};

// Marks every entity of an editor panel's hidden preview stage: the previewed character subtree
// far below the scene (Editor/PreviewStage.h's origin slots) and the panel's own camera entity. It
// always travels with SceneTransientTag, which keeps the stage out of a saved scene; this one is
// what the editor's other entry points test (its pool, whatever the owner) to keep every stage out
// of the Outliner, viewport picking, Save-as-Prefab / Copy, the main-viewport chain overlay and a
// chain preview's scene targets. `owner` says whose stage it is (a census counts its own). Editor
// host only in practice; nothing serializes it.
struct PreviewStageTag
{
    static constexpr std::uint8_t kChainPreview   = 1;   // the Chain Physics Preview panel
    static constexpr std::uint8_t kSkeletonEditor = 2;   // the Skeleton Editor's Animation host
    static constexpr std::uint8_t kAnimGraphEditor = 3;
    static constexpr std::uint8_t kUIEditor       = 4;   // a UI Editor asset tab
    std::uint8_t owner = 0;
};

// RenderLayer: bitmask for camera layer filtering. Bit 0 = default scene layer; a camera renders
// only entities whose mask overlaps its own layer mask.
struct RenderLayer
{
    uint32_t mask = 1u;
};

// LocalAabb: model-space axis-aligned bounding box, set at load time. TransformSystem combines it
// with GlobalTransform into WorldAabb each frame.
struct LocalAabb
{
    DirectX::XMFLOAT3 min = { -0.5f, -0.5f, -0.5f };
    DirectX::XMFLOAT3 max = {  0.5f,  0.5f,  0.5f };
};

// WorldAabb: world-space axis-aligned bounding box, recomputed each frame from LocalAabb. Used by
// FrustumCullSystem / the BVH for per-entity visibility tests.
struct WorldAabb
{
    DirectX::XMFLOAT3 min = { -0.5f, -0.5f, -0.5f };
    DirectX::XMFLOAT3 max = {  0.5f,  0.5f,  0.5f };
};

// MeshLibRef: reference to one mesh inside a MeshLibrary (.meshlib file), one per mesh entity.
// `libHandle` names the library, `meshId` indexes its MeshLibraryEntry[] table; geometry range,
// AABB and default material are fetched through the entry at draw time.
struct MeshLibRef
{
    Resource::Handle libHandle = {};   // ResourceType::MeshLibrary
    uint32_t         meshId    = 0;    // index into library's entries[]

    // MeshManager's O(1) cache of the resolved mesh-descriptor slot. Valid only while
    // cachedGeneration == MeshManager::GetMeshLibDescGeneration(); a global invalidation
    // (OnWorldClear) bumps the generation, forcing a re-lookup without walking the ECS to patch
    // every ref.
    mutable uint32_t cachedDescSlot   = 0xFFFFFFFFu;
    mutable uint32_t cachedGeneration = 0;

    bool IsValid() const { return libHandle.IsValid(); }
};

// SceneNodeTag: marks a structural node entity (no geometry), as opposed to a mesh entity (has a
// MeshLibRef).
struct SceneNodeTag {};

// MeshSourcePath: the absolute .imsh path a mesh entity was loaded from. Written by
// SceneInstanceLoader, read by PrefabSerializer.
struct MeshSourcePath
{
    std::string path;
};

// Source path for the .iscn scene that created this skinned character.
// Set during SceneInstanceLoader::Load; used by PrefabSerializer to reconstruct.
struct SceneSourcePath
{
    std::string path;  // absolute path to .iscn file
};

// Single-clip source path (.ianim) of old AnimRef scene/prefab data, kept only so that data
// migrates (AnimationClipSystem removes it); authoring uses AnimationSetComponent.
struct AnimationSourcePath
{
    std::string path;  // absolute path to .ianim file
};

// Source path of the .ipfb prefab that instantiated this entity subtree, set on the instance root
// by Resource::LoadPrefab. SaveScene then stores the instance as a pure reference (N line +
// "PrefabRef: path=..") instead of duplicating the tree, so prefab edits reach every referencing
// scene on its next load. Remove this component to detach the instance (it then saves as plain
// entity data).
struct PrefabSourcePath
{
    std::string path;  // path to the .ipfb file
};

// PrefabInstanceMember: back-pointer from every entity Resource::LoadPrefab created to the
// instance root that owns it. Runtime only: deliberately not in the ComponentSerializerRegistry,
// because LoadPrefab re-establishes it on every instantiation.
//
// SaveScene uses it to skip content the .ipfb re-creates on load. A Children walk from the root is
// not enough: the followers SavePrefab's CollectLinkedFollowers pulls in (an equipped weapon, its
// trail, its spark emitter) attach by FollowSocket / FollowEntity, not parenting, so they are
// top-level nodes inside the .ipfb and Children-less siblings of the root in the world; saved
// again as scene rows they would spawn twice. A tag rather than a re-derived follow closure stays
// exact when equipment code rebinds a follower at runtime, and never swallows a follower the user
// authored in the scene.
//
// Honoured only while `root` is alive and still carries PrefabSourcePath: detaching the instance
// turns its members back into plain scene data, like the root.
struct PrefabInstanceMember
{
    Entity root = NullEntity;
};

// Source path for the material (.imat) that this entity's MaterialComponent was loaded from.
// When present, prefab/world serialization saves a reference instead of inline data.
struct MaterialSourcePath
{
    std::string path;  // path to .imat file
};

// MaterialInstanceInfo: what the entity's material file said about itself. Present only when
// MaterialSourcePath points at a file with a `parent` key, so master-backed entities pay nothing.
//
// The resolved MaterialComponent cannot say "is this value mine or my parent's" (after the merge an
// override and an inherited value are the same bytes), so anything that re-saves an instance or
// draws Unreal's per-parameter override checkbox reads it here.
//
// Deliberately not serialized: MaterialSourcePath names the file and the file is the truth, so a
// persisted copy could drift; it is refilled from Resource::MaterialLoadInfo on every load. Also
// deliberately not part of MaterialComponent, a dense pool value cloned by World::CloneState on
// entering Play and read on the renderer's gather path: keeping provenance out of it keeps it out
// of matHash, so identical values from different files still share one GPU material slot.
struct MaterialInstanceInfo
{
    std::string                     parentPath;             // "" => source is a master
    std::unordered_set<std::string> overrides;              // keys this file authors
    std::vector<std::string>        unsetKeys;              // inherited keys removed
    bool                            explicitFormat = false; // file carried imatfmt = 2
    bool                            parentResolved = true;  // false => chain is broken
};

// Push a LoadMaterial result onto @p e, adding or removing MaterialInstanceInfo
// so its PRESENCE always means "this entity's source is an instance".
//
// Every path that loads a material into an entity must call this, or a later
// re-save has no override set to write and silently falls back to guessing one.
inline void ApplyMaterialLoadInfo(World& w, Entity e,
                                  const Resource::MaterialLoadInfo& info)
{
    if (info.parentPath.empty())
    {
        // Source is a master. Remove any stale component from a previous source:
        // re-pointing an entity from an instance to a master must not leave it
        // claiming overrides that belonged to the old file.
        if (w.GetComponent<MaterialInstanceInfo>(e))
            w.RemoveComponent<MaterialInstanceInfo>(e);
        return;
    }

    MaterialInstanceInfo mi;
    mi.parentPath     = info.parentPath;
    mi.overrides      = info.overrideKeys;
    mi.unsetKeys      = info.unsetKeys;
    mi.explicitFormat = info.explicitFormat;
    mi.parentResolved = info.parentResolved;
    if (auto* existing = w.GetComponent<MaterialInstanceInfo>(e))
        *existing = std::move(mi);
    else
        w.AddComponent<MaterialInstanceInfo>(e, std::move(mi));
}

// MaterialOverride: per-entity material parameter overrides
// (docs/design/ecs_scene_serialization_architecture.md).
//   - stores only the properties that differ from the base .imat;
//   - string-keyed, so a shader that adds or drops a parameter degrades instead of failing:
//     unknown keys are skipped;
//   - entities without overrides have no component and pay nothing.
// The scene stores the key=value pairs inline; Resource::ApplyMaterialOverride applies the bag
// onto the resolved MaterialComponent at load.
struct MaterialOverride
{
    // Typed variant for material property values
    struct Value
    {
        enum Type : uint8_t { Float, Vec4, Int, Bool, TexRef };
        Type type = Float;
        union {
            float f;
            float v4[4];
            int   i;
            bool  b;
        } data = {};
        std::string texPath; // only if type == TexRef

        static Value MakeFloat(float v)        { Value r; r.type = Float; r.data.f = v; return r; }
        static Value MakeVec4(float x, float y, float z, float w)
        { Value r; r.type = Vec4; r.data.v4[0]=x; r.data.v4[1]=y; r.data.v4[2]=z; r.data.v4[3]=w; return r; }
        static Value MakeInt(int v)            { Value r; r.type = Int; r.data.i = v; return r; }
        static Value MakeBool(bool v)          { Value r; r.type = Bool; r.data.b = v; return r; }
        static Value MakeTex(const std::string& p) { Value r; r.type = TexRef; r.texPath = p; return r; }
    };

    // Persistent property bag: key = parameter name, value = override
    std::unordered_map<std::string, Value> props;

    bool Has(const std::string& key) const { return props.count(key) > 0; }
    void Set(const std::string& key, Value v) { props[key] = std::move(v); }
    void Remove(const std::string& key) { props.erase(key); }
    bool IsEmpty() const { return props.empty(); }
};

// MaterialSpawnDefault: what the loader gave this entity, before any user edit. Runtime only
// (absent from the serializer registry, never round-trips): it lets a save tell "the artist
// changed this part's material" from "the model or prefab already said so" and write a diff.
//
// A component rather than a re-read of the source file at save time, because:
//   * it answers "what was this entity given", not "what does the file say now"; after a
//     mid-session model re-import only this is right;
//   * a re-read of the .iscn is blind to the prefab layer (a .ipfb MaterialRef row that re-pointed
//     a part), so prefab-authored bindings would read as user edits;
//   * a .iscn has no MaterialOverride concept, so a re-read cannot express the value-bag baseline.
//
// Stamped at SceneInstanceLoader::SpawnSingleNode (the .iscn F-line `mat=`) and again by
// LoadPrefab after the .ipfb's own MaterialRef / MaterialOverride rows: always the last thing a
// loader decided. Survives Play -> Stop with the world snapshot; material hot reload never touches
// it (ReloadMaterialsAffectedBy re-reads the same path and writes neither MaterialSourcePath nor
// MaterialOverride).
struct MaterialSpawnDefault
{
    std::string      path;   // the MaterialSourcePath the loader assigned ("" = none)
    MaterialOverride ovr;    // the override bag the loader applied (usually empty)
};

