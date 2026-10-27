#pragma once

// PrefabSerializer: saves and loads an ECS entity tree as a .ipfb file.
//
// Blob: [AssetHeader (24 B)] [PrefabMetadata (16 B)] [line-based text payload, NUL-terminated]
//
// Text payload (components through ComponentSerializerRegistry):
//   N idx=<i> parent=<j|-1> tx= ty= tz= qx= qy= qz= qw= sx= sy= sz= [msec=<key>] name=<encoded>
//     Tag: key=val key=val ...      (2-space indented, one line per component)
//
// Example:
//   N idx=0 parent=-1 tx=0 ty=0 tz=0 qx=0 qy=0 qz=0 qw=1 sx=1 sy=1 sz=1 name=PointLight
//     LightData: type=1 radius=10.0000 intensity=1.0000 color=1_1_1 dir=0_-1_0 spotAngle=0.5236
//     Billboard: mode=2 worldSize=1.0000
//     MeshRef: path=asset/mesh.imsh
//     MaterialRef: path=asset/mat.imat
//
// Component kinds: INLINE (data in the file), REF (a path the resource system loads), DERIVED
// (runtime). Names and paths percent-encode space (%20) and % (%25).

#include "ECS/ECS.h"
#include "ECS/Guid.h"
#include "ECS/HierarchyComponents.h"   // MaterialOverride (PrefabNodeView carries one)
#include <memory>
#include <string>
#include <vector>

class Renderer;

namespace Resource
{
    class AssetManager;
    class AnimationClipSystem;

    // Serializes the subtree under @p root into a .ipfb at @p path, with the skinned-mesh source
    // paths (SceneSourcePath, AnimationSourcePath) when present. Returns true on success.
    // @p savedNodes (optional) receives the entities written, in node-index order: the subtree in
    // DFS pre-order, then the followers it auto-includes (CollectLinkedFollowers) - the set a
    // LoadPrefab of the file makes instance members, which linking the source to the prefab tags.
    bool SavePrefab(Entity root, World& world, const std::string& path,
                    std::vector<Entity>* savedNodes = nullptr);

    // Instantiates a .ipfb into @p world; returns the root, or NullEntity on failure. Mesh GPU
    // buffers load and deduplicate through @p assetMgr (path-cached). With @p renderer and
    // @p animClipSys, a skinned prefab respawns its stored .iscn and authored AnimationSet (an old
    // single AnimRef migrates to one).
    //
    // @p instanceSeed: the identity seed of this instantiation. A .ipfb bakes every node's
    // GuidComponent, so all are remapped before any entity exists (two copies would otherwise
    // collide in GuidRegistry): the root takes the seed itself, every other node
    // Guid::Derive(bakedGuid, seed), so the same seed reproduces the same identities. Callers that
    // persist an instance (LoadScene's PrefabRef rows, the save-game respawn) pass the stored root
    // Guid; nullptr draws a random seed, which becomes the root Guid the scene writes, so the next
    // load reproduces this instantiation.
    // Exception: a seed equal to the file's baked root Guid is the instance the file was saved from
    // (Editor.SavePrefab {link = true}); it keeps every baked Guid, so the references to its
    // members hold.
    Entity LoadPrefab(const std::string& path, World& world, AssetManager& assetMgr,
                      Renderer* renderer = nullptr,
                      AnimationClipSystem* animClipSys = nullptr,
                      const ECS::Guid* instanceSeed = nullptr);

    // ==== A prefab prepared for a preload ====
    // PreparePrefabForSpawn runs LoadPrefab's read-and-parse exactly as a runtime spawn does (the
    // bytes through the spawn template, Resource/PrefabTemplate.h; the references re-resolved by
    // guid; the records kept for that resolution of the text), so the spawn then reads and parses
    // nothing. @p out receives what the preload prepares next: the models the SceneRef blocks name
    // (lowest node first) and the materials the MaterialRef blocks name (canonical, unique).
    // Main thread (the template is), like LoadPrefab. Returns false when it does not read.
    struct PrefabSpawnRefs
    {
        std::vector<std::string> scenes;
        std::vector<std::string> materials;
    };
    bool PreparePrefabForSpawn(const std::string& path, PrefabSpawnRefs& out);
    // The same in two halves, so the parse leaves the main thread. ParsePrefabForSpawn: any
    // thread (a preload worker), bytes already read - validates, repairs the references by guid,
    // parses the records and collects the refs; nullptr when they do not parse (logged).
    // PutParsedPrefabForSpawn: main thread - puts those records into the spawn template's cache
    // for that resolution of the text (as PreparePrefabForSpawn would) and the refs into @p out.
    struct PrefabSpawnParse;
    std::shared_ptr<PrefabSpawnParse> ParsePrefabForSpawn(const std::string& path, const std::vector<uint8_t>& bytes);
    void PutParsedPrefabForSpawn(const std::string& path, PrefabSpawnParse& parsed, PrefabSpawnRefs& out);

    // ==== Read-only node view (asset preview) ====
    // A .ipfb's node tree as data: no World, no entities, no GuidRegistry hook, no GPU upload. The
    // asset preview composes its draw packets from this plus
    // SceneInstanceLoader::ReadMeshPlacements instead of spawning a private World.
    // Only the fields a preview needs are lifted; the rest stays unread on purpose: a reader that
    // grew a field per component would become a second, silently diverging deserialiser.
    struct PrefabNodeView
    {
        int   idx    = -1;
        int   parent = -1;
        // Composed through the parent chain. Row-major.
        float world[16] = { 1.f, 0.f, 0.f, 0.f,
                            0.f, 1.f, 0.f, 0.f,
                            0.f, 0.f, 1.f, 0.f,
                            0.f, 0.f, 0.f, 1.f };
        std::string name;
        // "SceneRef: path=" - the .iscn this node re-spawns. LoadPrefab maps
        // the spawned subtree onto node indices idx, idx+1, ... in DFS order,
        // which is what SceneInstanceLoader::MeshPlacement::dfsIndex counts.
        std::string sceneRefPath;
        // "MaterialRef: path=" - this node's material OVERRIDE. On a node that
        // maps onto a .iscn mesh entity it replaces the F-line's `mat=`; on a
        // MeshHandle node it IS the material.
        std::string materialRefPath;
        // "MaterialInline: data=" - a WHOLE material serialized into the prefab,
        // for a node that has no MaterialSourcePath. Percent-DECODED here, i.e.
        // the same key=value material text Resource::ApplyMaterialTextInPlace
        // takes. Empty when the node has no inline block.
        std::string materialInlineText;
        // "MaterialOverride: k=t:v ..." - the property-bag diff applied on top of whatever the ref /
        // inline / .iscn `mat=` resolved to.
        // MaterialInline and MaterialOverride are lifted because LoadPrefab applies them, in this order:
        //   .iscn `mat=`  ->  MaterialRef  ->  MaterialInline  ->  MaterialOverride
        // (the first three in the component-block walk's registration order, which is how SavePrefab
        // emits them; the override is LoadPrefab's post-process, so it has the last word).
        // ComposePrefabAsset replays exactly that order.
        MaterialOverride materialOverride;
        bool  hasMaterialOverride = false;
        // "MeshHandle: gpuMeshID=" - a primitive. -1 when absent.
        int   gpuMeshId = -1;
        // True when the node carries a "MeshRef:" line. Nothing resolves it (MeshSourcePath is a
        // display hint), but its presence marks the node as a per-mesh override record rather than a
        // wrapper empty.
        bool  hasMeshRef = false;
        // `msec=` on the N line: the SectionKey of the model part this node was saved as (0: not a
        // part, or saved without keys). See MatchPrefabPartsByKey.
        uint64_t sectionKey = 0;
    };

    // ==== A prefab's model parts, by section key ====
    // A prefab applies its per-part blocks (materials, colliders...) to a spawned model by DFS
    // ordinal (node idx - the SceneRef node's idx). SavePrefab also writes each part's SectionKey
    // (`msec=` on its N line); a re-import that reorders the parts moves the ordinals, not the keys.
    // The match: the k-th saved node with key K (idx order) is the k-th live part with key K (DFS
    // order); one section placed twice is two parts. LoadPrefab and the prefab preview
    // (ComposePrefabAsset) both apply it, so the two agree.
    struct PrefabPartMatch
    {
        std::vector<std::pair<int, int>> nodeToLive;   // saved node idx -> live DFS ordinal
        std::vector<int>                 goneNodes;    // keyed saved nodes no live part matches
        int                              moved = 0;    // matched at another ordinal than (idx - first)
    };
    // @p saved: (node idx, key) of the keyed saved nodes of ONE SceneRef's saved
    // subtree (any order); @p first: that SceneRef node's idx (ordinal 0);
    // @p liveKeys: per live DFS ordinal, the key of the part there (0: not a part).
    PrefabPartMatch MatchPrefabPartsByKey(const std::vector<std::pair<int, uint64_t>>& saved, int first,
                                          const std::vector<uint64_t>& liveKeys);
    // The SectionKey of a spawned model part: its MeshLibRef entry in its
    // MeshSourcePath library (MeshLibKeyTableAt). 0 when it is not a library part.
    uint64_t PrefabPartKeyOf(World& world, Entity e);

    // How many nodes of each kind the preview walk declined. Not a field per component and not a
    // second deserialiser: a tally of the component lines' tag tokens, which parses no payload and
    // adds no PrefabNodeView field.
    // Counted per node, not per component: a VFX node carries ParticleRender + ParticleEmit
    // (+ ParticleFlock / ParticleAmbient) and an artist counts one emitter. The tag spellings are
    // the serializers' own (RegisterAllComponentSerializers); ReadPrefabNodes checks its table
    // against ComponentSerializerRegistry on first use, so a renamed tag reports instead of
    // counting zero.
    // In: a node whose look comes from something other than a static mesh - particles, trails,
    // lights (LightData, VolumetricLight, SunLight, MoonLight, TODLight), decals, billboards,
    // surfaces drawn by a pass the preview does not own (Grass, Terrain, Water) and WorldUI.
    // Out on purpose: Video (the mesh is drawn, without the video), WaterRipples (counted through
    // its Water), environment / post-process components (they describe the world a prefab is
    // placed in; the preview lights it with the studio rig), and an "other" bucket (it would count
    // Guid, Visibility and every future block).
    struct PrefabSkipCounts
    {
        uint32_t emitters   = 0;   // ParticleRender|ParticleEmit|ParticleFlock|
                                   // ParticleAmbient|ParticleEmitter
        uint32_t trails     = 0;   // Trail|Beam
        uint32_t lights     = 0;   // LightData|VolumetricLight|SunLight|
                                   // MoonLight|TODLight
        uint32_t decals     = 0;   // Decal
        uint32_t billboards = 0;   // Billboard|BillboardFX
        uint32_t surfaces   = 0;   // Grass|Terrain|Water
        uint32_t worldUI    = 0;   // WorldUI|WorldUIBar|WorldUIImage|WorldUIText
        uint32_t Total() const
        {
            return emitters + trails + lights + decals + billboards +
                   surfaces + worldUI;
        }
    };

    // Parses a .ipfb's node tree, read-only, through AssetFS (so it works on a packed build).
    // Returns false when the file cannot be read or is not a prefab container. `$g_` reference
    // tokens resolve exactly as in LoadPrefab, so a moved asset reads the same either way.
    // @p outSkipped (optional) receives the PrefabSkipCounts tally.
    bool ReadPrefabNodes(const std::string& path, std::vector<PrefabNodeView>& out,
                         PrefabSkipCounts* outSkipped = nullptr);

    // The DFS order a prefab's per-part data is keyed by, exported so a self-check compares
    // against it instead of re-deriving it. LoadPrefab runs this walk over the entities
    // SceneInstanceLoader::Load spawned and applies every saved component block by position
    // (entityMap[sceneIdx + i] = loadedEntities[i]); -preview_asset_verify uses it to prove that
    // SceneInstanceLoader::ReadMeshPlacements agrees with the live spawn. Pre-order over
    // Children, cycle-safe.
    void CollectPrefabSubtreeDFS(World& world, Entity root, std::vector<Entity>& out);
}
