#pragma once

// UIAsset - the .iui UI asset: a Canvas UI tree as canonical JSON. A spawned asset
// IS Cv* entities, laid out and painted like any other; this is the codec between the
// file and those entities, and it is a RENAMING layer over the Cv* component
// serializers (Resource/ComponentSerializers.cpp): every block of a node holds the
// serializer's own key / value pairs and is spawned through the serializer's own
// deserializer - so a node spawned from a .iui and the same node loaded from an
// .iscene are the same component, defaults included.
//
//   { "format": "iui", "version": 1, "root": <node> }
//   node: { "id", "name", "order"?, "canvas"? (the root only), "rect" (every node but a
//           canvas root), "clip"?, "image"?, "text"?, "interact"?, "sprite"?, "children"? }
//
// The hierarchy is the nesting; sibling order is the array order ("order" only for a
// specific siblingIndex, as a backdrop's -1000). Canonical text: keys in this fixed
// order, a block on one line, every value equal to the deserializer's default left out,
// numbers printed as the scene serializer prints them (float, 6 significant digits).
// An edit of one field changes one line.
//
// A node's GUID is derived from (the asset, its id, a per-spawn seed): authors never
// hold a GUID, and two spawns never tangle (as a .ipfb without Guid blocks does: it
// hands every copy the same parentGuid).

#include "ECS/ECS.h"
#include "ECS/Guid.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Resource { class AssetManager; }

namespace UI
{
    // One block of a node: a Cv* serializer tag and its key / value pairs, the
    // serializer's own text (percent-encoded strings, '_'-joined vectors).
    struct UIAssetBlock
    {
        std::string tag;                                       // "CvRect", "CvImage", ...
        std::vector<std::pair<std::string, std::string>> kv;   // serializer key -> value text
        const std::string* Get(std::string_view key) const;
    };

    struct UIAssetNode
    {
        std::string id;                 // unique in the file: Engine.FindUINode's key, the GUID key
        std::string name;               // the entity's name
        bool        hasOrder = false;   // an explicit siblingIndex; else the array position
        int         order    = 0;
        std::vector<UIAssetBlock> blocks;   // canonical block order (canvas, rect, clip, image, text, interact, sprite)
        std::vector<UIAssetNode>  children;
        const UIAssetBlock* Block(std::string_view tag) const;
        UIAssetBlock*       Block(std::string_view tag);
    };

    struct UIAsset
    {
        int         version = 1;
        UIAssetNode root;
        std::size_t NodeCount() const;
    };

    // A problem in a file: where ("root.children[2].rect.size") and what.
    struct UIAssetIssue
    {
        std::string where;
        std::string message;
    };

    // ==== The codec: no World, no device ====
    // The limits of one asset: a .iui is untrusted input, every walk of a tree
    // recurses per level on a 1 MB stack, and a spawn walks every node.
    inline constexpr int         kUIAssetMaxDepth = 64;     // nesting levels below the root
    inline constexpr std::size_t kUIAssetMaxNodes = 4096;   // nodes in one asset
    // Parse .iui text. Strict: bad JSON, an unknown key, a wrong type, a value the Cv*
    // authoring contract refuses, a missing / malformed / duplicate id, a canvas off the
    // root, a node with no rect, nesting or a node count past the limits above - each an
    // issue; false when there is any.
    bool ParseUIAsset(std::string_view text, UIAsset& out, std::vector<UIAssetIssue>& issues);
    // The canonical text of @p asset (ends with a newline).
    std::string WriteUIAsset(const UIAsset& asset);
    // Every asset path a node references (image texture, text font, text customShader).
    struct UIAssetRef
    {
        std::string id;      // the node
        std::string field;   // "image.texture", "text.font", "text.customShader"
        std::string path;    // decoded
    };
    std::vector<UIAssetRef> UIAssetRefs(const UIAsset& asset);
    // A node id made from @p name: lower case, [a-z0-9_] only, not empty.
    std::string UIAssetIdFromName(std::string_view name);

    // ==== Assets and entities ====
    // The world subtree under @p root as an asset, every node's Cv* components through
    // their serializers. Ids: @p ids when it names the entity (a designer's table), else
    // the node's UIAssetNodeId, else made from its name - unique in the file either way.
    // @p idsOut: each captured entity and the id written for it (a UI Editor save pins
    // those on nodes that had none, so a later rename does not change the file's id).
    bool CaptureUIAsset(World& world, Entity root, UIAsset& out, std::string* why,
                        const std::vector<std::pair<Entity, std::string>>* ids = nullptr,
                        std::vector<std::pair<Entity, std::string>>* idsOut = nullptr);

    // A spawned node's id in its asset - runtime only, never saved (Engine.FindUINode).
    struct UIAssetNodeId { std::string id; };

    struct UISpawnOptions
    {
        Entity      parent = NullEntity;    // a canvas or a UI node; NullEntity = an unattached root
        ECS::Guid   seed{};                 // invalid = a fresh one per spawn
        std::string assetKey;               // the GUIDs' namespace: the asset's canonical path
        Resource::AssetManager* assets = nullptr;   // handed to the deserializers
    };
    // Spawn @p asset into @p world through the scene loader's deserializers, with UIParent
    // edges and siblingIndex (order, else the array position). A canvas root stands alone
    // (a parent is refused); a rect root goes LAST under opt.parent. The root, or
    // NullEntity with @p why (nothing is left behind). @p idsOut: id -> entity.
    Entity SpawnUIAsset(World& world, const UIAsset& asset, const UISpawnOptions& opt, std::string* why,
                        std::vector<std::pair<std::string, Entity>>* idsOut = nullptr);
    // The node of the instance under @p root whose asset id is @p id; NullEntity: none.
    Entity FindUINode(const World& world, Entity root, std::string_view id);

    // ==== Files ====
    // Read and parse a .iui (AssetFS: the mounted pak, else the disk), cached by path and
    // re-read when the file's size or write time changes. nullptr with @p why (the
    // issues, joined) on failure.
    std::shared_ptr<const UIAsset> LoadUIAsset(const std::string& path, std::string* why);
    // The hot-reload watcher's hook: forget @p path (the next load reads the file).
    void InvalidateUIAsset(const std::string& path);
    // Bumped whenever a file is read into the cache or dropped from it: UpdateDocuments
    // (UI/UIDocument.h) looks at its documents' assets again only when it moved.
    std::uint64_t UIAssetGeneration();
}
