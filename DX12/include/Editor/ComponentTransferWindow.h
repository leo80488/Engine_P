#pragma once

// ComponentTransferWindow - side-by-side component diff + copy between two
// targets, each EITHER a live entity OR a .ipfb prefab file. The gesture it
// exists for: give a new character prefab the chain physics / face shading /
// audio setup already tuned on another, without retyping every field into the
// Inspector or hand-editing a .ipfb.
//
// Model:
//   Target A --+
//              +--> one merged row per component TYPE, with a status
//   Target B --+    (Only A / Only B / Both / Both identical), then a
//                   per-row [->] / [<-] to push that component across.
//
// Transfer paths, in the order they are attempted per component:
//
//   1. SERIALIZER BLOB - the "  Tag: k=v ..\n" text produced by the
//      ComponentSerializerRegistry. The engine's canonical component transfer:
//      .iscene save, .ipfb save, SaveGame's PersistentTag snapshot and the
//      editor's own undo all route through it, and its `deserialize`
//      re-resolves asset PATHS through the AssetManager rather than aliasing
//      the source's already-loaded handles. It is also the ONLY path that can
//      touch a prefab, which is a text file with no live components at all.
//
//   2. DEEP VALUE COPY - IComponentPool::CloneComponent/RestoreComponent via
//      World::Clone/RestoreComponentByType, i.e. the same ComponentCloner<T>
//      pass Edit <-> Play and structural undo use. Reaches the ~dozen runtime
//      components that have a pool but NO serializer (SkeletonComponent,
//      MeshLibRef, MorphComponent, AnimationComponent, ...). Entity <-> entity
//      only, and opt-in (see the Warn policy below).
//
// UNDO is uniform and independent of which path ran: both sides are captured
// as type-erased ComponentCloner deep copies and replayed through a
// ValueEditAction (absence is a RestoreFn that removes). One EditMacro per
// press, so a 6-component transfer is one Ctrl+Z.
//
// PREFAB WRITE-BACK IS NOT UNDOABLE: it edits a file on disk, outside the
// EditHistory. It writes a `.ipfb.bak` next to the original first and requires
// an explicit confirmation.

#include "ECS/ECS.h"

#include <cstdint>
#include <string>
#include <typeindex>
#include <vector>

class ComponentSerializerRegistry;

namespace Editor::ComponentTransfer
{

// ==== PrefabDoc - a parsed, EDITABLE .ipfb ====
// Layout on disk (see Resource/PrefabSerializer.h + AssetHeader.h):
//   [AssetHeader 24 B][PrefabMetadata 16 B][line text][NUL]
// and the text is node lines
//   N idx=<i> parent=<j|-1> tx=.. .. name=<percent-encoded>
// each followed by its 2-space-indented component lines
//     Tag: key=val key=val
//
// The payload is kept as a VECTOR OF LINES plus indices into it, not a
// component map, because:
//   * write-back re-emits every untouched line byte-for-byte, so patching one
//     component cannot perturb node transforms, ordering, or the prefab-local
//     link records (`PrefabFollow*` / `PrefabSkeletonRef`, whose `target=<idx>`
//     values are positional and would not survive a re-serialize);
//   * a component may legitimately occupy SEVERAL lines with the same tag
//     (ScriptComponent emits one line per script slot and relies on
//     deserialize being called once per line), so "the blob for this tag" is a
//     line set, not a line.
struct PrefabDoc
{
    struct Comp
    {
        std::string         tag;
        std::vector<size_t> lines;   // indices into PrefabDoc::lines, ascending
    };

    struct Node
    {
        int                 idx    = -1;   // the file's own idx= value
        int                 parent = -1;
        std::string         name;
        size_t              headerLine = 0;   // index of this node's "N ..." line
        size_t              endLine    = 0;   // one past this node's last line
        std::vector<Comp>   comps;
    };

    std::string              path;
    std::vector<std::string> lines;       // payload text split on '\n' (no terminators)
    std::vector<Node>        nodes;
    std::vector<uint8_t>     header;      // original AssetHeader + PrefabMetadata bytes
    bool                     valid = false;

    void Clear() { path.clear(); lines.clear(); nodes.clear(); header.clear(); valid = false; }
};

// Read + parse a loose .ipfb. Deliberately uses a plain ifstream rather than
// AssetFS: a packed .ipak entry is not editable, and silently transferring
// INTO one would write a file the engine never reads back.
bool LoadPrefabDoc(const std::string& path, PrefabDoc& out, std::string* err);

// Re-emit the document. Copies the original to "<path>.bak" first (overwriting
// a previous .bak), then rewrites header + patched PrefabMetadata + text + NUL.
// nodeCount is carried through unchanged: this API never adds or drops nodes.
bool SavePrefabDoc(const PrefabDoc& doc, std::string* err);

// Whole blob (all lines, each newline-terminated) for `tag` on nodes[nodeIdx].
// Empty when the node does not carry that tag.
std::string GetNodeComponentBlob(const PrefabDoc& doc, size_t nodeIdx, const std::string& tag);

// Replace - or, when absent, append at the end of the node's block - every line
// for `tag` on nodes[nodeIdx]. `blob` is raw serializer text ("  Tag: ..\n",
// one or more lines). An empty blob removes the tag. Re-parses line indices
// afterwards, so held indices are invalidated by this call.
bool SetNodeComponentBlob(PrefabDoc& doc, size_t nodeIdx,
                          const std::string& tag, const std::string& blob);

// ==== Policy - what a transfer of this component type is allowed to do ====
// Blocked types are structural or identity state: moving them does not copy a
// property, it corrupts the scene graph or duplicates an identity. Warn types
// transfer fine mechanically but carry a meaning the user must confirm.
enum class Policy : uint8_t
{
    Free,      // ordinary authored data: transfer freely
    Warn,      // transferable, but says something the user must mean (see reason)
    Blocked,   // never transferable: hierarchy, identity, or derived-per-frame
};

// Which of the two transports a type is allowed to use. Independent of Policy:
// a component can be perfectly Free to transfer and still be lethal to raw-copy.
enum class Transport : uint8_t
{
    // Blob when a serializer exists; raw value copy ONLY when none does.
    Auto,
    // Never raw-copy: the component owns a GPU descriptor slot or a physics body,
    // and a second owner would free it out from under the first. The blob path is
    // safe for these because their deserializers reset the slot / body id to
    // "unallocated".
    BlobOnly,
    // Raw copy is the CORRECT path even though a serializer exists (the serializer
    // stands down, or does not carry everything).
    ValueOk,
};

struct PolicyInfo
{
    Policy      policy    = Policy::Free;
    Transport   transport = Transport::Auto;
    const char* reason    = nullptr;   // tooltip text; null for Free
};

PolicyInfo PolicyFor(std::type_index ti);

// The serializer tag for a component type, or nullptr when the type has no
// serializer registered (-> blob transfer impossible, prefab transfer
// impossible, value copy is the only path).
const std::string* TagForType(const ComponentSerializerRegistry& reg, std::type_index ti);

// Inverse lookup. `found` is set false when this build has no serializer under
// that tag: a .ipfb written by a build that had one, or the prefab-local
// `Prefab*` link records that are not registry components.
std::type_index TypeForTag(const ComponentSerializerRegistry& reg,
                           const std::string& tag, bool* found);

// True for the prefab-local link records SavePrefab writes with a "Prefab" tag
// prefix (`PrefabFollowTarget`, `PrefabSkeletonRef`, ...). The prefix exists so
// LoadPrefab's registry walk skips them; their values are POSITIONAL node
// indices, meaningless outside the file that produced them.
bool IsPrefabLocalTag(const std::string& tag);

// ==== Window state (owned by EditorLayer, drawn by RenderComponentTransferWindow) ====
enum class TargetKind : uint8_t { None, EntityTarget, PrefabTarget };

struct Target
{
    TargetKind   kind   = TargetKind::None;
    // Generation-checked: a target deleted while the window is open reports
    // "target is gone" instead of silently retargeting a recycled id.
    EntityHandle entity = NullEntityHandle;
    PrefabDoc    prefab;
    int          prefabNode  = 0;      // index into prefab.nodes
    // Prefab edits are batched in memory and written by an explicit [Save]:
    // one file rewrite per gesture instead of one per component, and the user
    // gets a chance to look at the diff before anything touches disk.
    bool         prefabDirty = false;

    bool IsEntity() const { return kind == TargetKind::EntityTarget; }
    bool IsPrefab() const { return kind == TargetKind::PrefabTarget; }
    void Clear()
    {
        kind = TargetKind::None; entity = NullEntityHandle;
        prefab.Clear(); prefabNode = 0; prefabDirty = false;
    }
};

// One merged row = one component type present on at least one side.
struct Row
{
    std::type_index type  = std::type_index(typeid(void));
    std::string     label;                 // Inspector display name, or the tag
    std::string     tag;                   // serializer tag ("" = none registered)
    bool            typeKnown = true;      // false: prefab tag unknown to this build
    bool            inA = false;
    bool            inB = false;
    bool            identical = false;     // both sides present and blob-equal
    bool            selected  = false;
    PolicyInfo      policy;
};

struct State
{
    Target           a, b;
    std::vector<Row> rows;
    bool             rowsDirty = true;   // rebuild the merged list next frame

    char             filter[64]      = {};
    bool             showBlocked     = false;
    bool             overwrite       = true;   // false = only fill gaps
    bool             allowValueCopy  = false;  // enable the no-serializer path
    bool             confirmPrefabWrite = false;

    std::string      status;
    bool             statusIsError = false;
};

} // namespace Editor::ComponentTransfer
