#pragma once

// MeshEditorWindow - the Mesh Editor: one Mesh of a model, its own view, what
// it is made of, and its material slots.
//
// What it shows. The Mesh through a Synthetic preview view of kind MeshAsset
// ("<model .iscn>#<MeshKey hex>" - the composition its Content Browser tile
// uses), with the view's own view mode (Lit, Unlit, Wireframe and the GBuffer
// channels: ViewDesc::viewMode, never the main viewport's), per-section
// visibility (solo / isolate: ViewDesc::partMask) and a picked section drawn
// over its dimmed neighbours (ViewDesc::highlightPart). Over the picture, CPU
// overlays projected like ChainPreviewOverlay's: bounds, pivot, a ground grid,
// sampled normals / tangents / bitangents. A UV Layout tab draws a section's
// UV triangles. The geometry for both comes from the file
// (Scene/MeshGeometry.h): MeshLibrary keeps no CPU copy.
//
// What it edits. The default material of each section - the model's F-line
// `mat=`, which every StaticMesh of the Mesh without a slot override draws -
// and the Mesh's asset collision (its BO / PR records). Edits are staged in the
// panel (its own 64-deep undo ring: Ctrl+Z / Ctrl+Y while the panel is
// focused, never the scene's EditHistory - a model file is not scene state)
// and written by Save (SceneInstanceLoader::WriteDefaultMaterials: the first
// F-line and every repeat of it, atomically; the collision below). A save then
// re-derives every StaticMesh of the model (MeshAssetRegistry::InvalidateModel),
// recomposes previews and re-tiles the Content Browser. Close / open-another
// with unsaved edits asks first.
//
// Shape keys (Editor/MeshShapeKeys.h). The Mesh's morph targets from the
// model's .imorph, a weight per target, drawn deformed in the picture (the
// panel's own morphed copies of the sections, the library's slots untouched),
// solo, and a heatmap of one target's |delta|. The weights are a preview,
// never staged or saved. Staged with the rest and written by Save: each
// target's metadata (category, default, range, label - the MK records) and the
// Mesh's presets (named weight sets - MP).
//
// Collision. Prims fitted to a section's or the whole Mesh's vertices
// (Physics/CollisionFit.h: box, sphere, capsule, k-DOP, convex hull), then
// edited by value. A k-DOP or a hull is a hull prim: its points wait in the
// staged state until Save appends them to the collision library beside the
// model (<model stem>_collision.meshlib, Resource::CollisionLibrary - an entry
// there is never moved or removed, positional V records name them), fills in
// its lib= / sec=, and replaces the Mesh's records
// (SceneInstanceLoader::ReplaceModelCollision); every placement in the world
// follows (MeshEditorContext::collisionChanged). Read-only for a packed model,
// one reached through a junction, one with C records (they win over asset
// collision on the parts they name until migrated) and a skinned Mesh (its
// collision is its skeleton's).
//
// Sockets. The Mesh's `SO mesh=<key>` records - named attachment points in its
// own space, staged with the rest, drawn in the picture (Show > Sockets) and
// written by Save (SceneInstanceLoader::ReplaceMeshSockets: this Mesh's records
// only); every StaticMesh of the Mesh publishes them
// (SceneInstanceLoader::DeriveMeshSockets). The Skeleton block (a skinned
// Mesh): the bones that move its vertices, one bone's weight heatmap
// (Editor/BoneWeightHeat, part k = section k), and the way to the Skeleton
// Editor.
//
// The preview-layer rules (AssetPreviewWindow.h's header) hold here too: the
// orbit is the panel's own state, the SRV is re-resolved every frame, the size
// is quantised + debounced, the view is released at the frame boundary when
// the panel stopped drawing, and everything that touches files or the
// Renderer's game-half state runs in ProcessPendingActions.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "Editor/BoneWeightHeat.h"      // a skinned Mesh's weight heatmap
#include "Editor/MeshShapeKeys.h"       // the Mesh's shape keys
#include "Resource/ModelCollision.h"    // the Mesh's asset collision
#include "Resource/ModelShapeKeys.h"    // the Mesh's MK / MP records
#include "Resource/ModelSockets.h"      // the Mesh's SO mesh= records
#include "Scene/MeshAssetRegistry.h"   // the model view (sections, default materials)

#include <DirectXMath.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class Renderer;
class IGraphicsDevice;
struct ImDrawList;

namespace Editor
{
    // What the panel needs from EditorLayer, which it cannot reach itself.
    struct MeshEditorContext
    {
        Renderer*        renderer    = nullptr;
        IGraphicsDevice* gfx         = nullptr;
        std::uint64_t    editorFrame = 0;     // EditorLayer::m_editorFrame (the harness clock)
        bool             playing     = false; // Play mode: saving a model is refused
        // World::Epoch(): a reload invalidates the mesh-descriptor slots the
        // composition resolved to, so the panel asks for it again.
        std::uint32_t    worldEpoch  = 0;
        bool             hasWorld    = false;
        // NotifyThumbnailDependencyChanged: a saved model re-tiles its browser items.
        std::function<void(const std::vector<std::string>&)> filesChanged;
        // Show a file in the Content Browser.
        std::function<void(const std::string&)> reveal;
        // The model's collision records (and its collision library) changed on disk:
        // the physics shape caches forget them, Static Mesh owners re-derive, every
        // placement in the world follows. Returns the part collider changes
        // (SceneInstanceLoader::RefreshAssetCollision).
        std::function<std::uint32_t(const std::string&)> collisionChanged;
    };

    class MeshEditorWindow
    {
    public:
        // ---- Overlays (Show) ----
        static constexpr std::uint32_t kShowBounds     = 1u << 0;
        static constexpr std::uint32_t kShowPivot      = 1u << 1;
        static constexpr std::uint32_t kShowGrid       = 1u << 2;
        static constexpr std::uint32_t kShowNormals    = 1u << 3;
        static constexpr std::uint32_t kShowTangents   = 1u << 4;
        static constexpr std::uint32_t kShowBitangents = 1u << 5;
        // A skinned Mesh's bind-pose skeleton (the model's K-line .iskel).
        static constexpr std::uint32_t kShowSkeleton   = 1u << 6;
        // The Mesh's staged collision: body prims, hull edges, trimeshes.
        static constexpr std::uint32_t kShowCollision  = 1u << 7;
        // The Mesh's staged sockets (`SO mesh=`).
        static constexpr std::uint32_t kShowSockets    = 1u << 8;
        static constexpr std::uint32_t kShowAll        = (1u << 9) - 1u;

        struct SaveReport
        {
            int           lines = 0;            // F-lines rewritten (materials)
            bool          collision = false;    // the Mesh's records were written
            bool          shapeKeys = false;    // the Mesh's MK / MP records were written
            bool          sockets = false;      // the Mesh's SO mesh= records were written
            std::uint32_t hullsAppended = 0;    // new entries in the collision library
            std::uint32_t refreshed = 0;        // part collider changes in the world
        };

        // ---- Collision (see the header comment) ----
        enum class CollisionAdd { Box, Sphere, Capsule, KDop10X, KDop10Y, KDop10Z, KDop18, KDop26, Convex };

        struct CollisionState
        {
            std::string                lock;          // "" = editable
            std::string                library;       // where Save puts new hulls (project-relative)
            Resource::MeshBodySetup    staged, saved;
            std::vector<std::uint32_t> pending;       // [prim] points waiting for Save (0 = none)
            std::vector<std::uint32_t> shown;         // [prim] points / vertices the overlay draws
            int                        selected = -1;
            bool                       dirty = false;
            std::uint32_t              overlayLines = 0;   // collision lines of the last overlay
        };

        // ---- The collision gizmo ----
        enum class GizmoOp { Off, Translate, Rotate, Scale };

        // ---- The skeleton and its weights ----
        struct SkinBone
        {
            int           bone = -1;
            std::string   name;
            std::uint32_t vertices = 0;   // this Mesh's vertices it moves at all
        };

        // ---- Scripted surface (Lua Editor.MeshEditor*, the harness) ----
        struct SectionState
        {
            std::uint32_t entry = 0;
            std::uint64_t key   = 0;
            std::string   slot;
            std::string   material;      // staged
            std::string   saved;         // on disk
            bool          editable = false;
            bool          visible  = true;
            std::uint32_t triangles = 0, vertices = 0;
        };
        struct StateView
        {
            struct ShapeKeyState
            {
                std::string   name;
                float         weight = 0.f;
                std::uint32_t sections = 0;
                float         maxDelta = 0.f;
                Resource::ShapeKeyMeta meta;    // staged (defaults when it has no MK)
            };

            bool          open = false, loaded = false, dirty = false;
            std::string   model, ref, meshName, error;
            std::uint64_t meshKey = 0;
            std::uint32_t viewId = ~0u, lease = 0, viewMode = 0, show = 0;
            std::uint64_t visibleMask = ~0ull, renders = 0;
            int           selected = -1;
            std::uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
            std::uint32_t pendingW = 0, pendingH = 0;   // the size after the resize debounce (== render: none waits)
            std::uint32_t triangles = 0, vertices = 0, stride = 0, uvChannels = 0;
            bool          tangents = false, colors = false, skinned = false, morph = false,
                          identityFromFile = false;
            std::uint64_t geometryBytes = 0;
            float         boundsMin[3] = { 0.f, 0.f, 0.f }, boundsMax[3] = { 0.f, 0.f, 0.f };
            std::string   skeleton;             // the model's .iskel ("" = none / not skinned)
            std::uint32_t skeletonBones = 0;
            std::size_t   undoDepth = 0, redoDepth = 0;
            std::uint32_t overlayLines = 0;   // what the last overlay draw emitted
            // UV Layout (the last computed): channel, section (-1 all), whole
            // triangles in it, drawn (after the 50k sample), UV bounds.
            int           uvChannel = 0, uvSection = -1;
            std::uint32_t uvTriangles = 0, uvDrawn = 0;
            float         uvMin[2] = { 0.f, 0.f }, uvMax[2] = { 0.f, 0.f };
            bool          uvOutside01 = false;
            std::vector<SectionState> sections;

            // Shape keys: the .imorph read, the sections that carry targets, whether the
            // file has normal deltas, solo / heatmap target (-1 none), the copies'
            // revision, whether anything is shown (active) and the copies carry it
            // (live), and each target.
            std::string   shapeKeyLock;
            bool          shapeKeysDirty = false;
            std::vector<Resource::ShapeKeyPreset> shapeKeyPresets;   // staged
            std::string   shapeKeyFile;
            bool          shapeKeyNormals = false, shapeKeyActive = false, shapeKeyLive = false;
            std::uint32_t shapeKeySections = 0;
            int           shapeKeySolo = -1, shapeKeyHeatmap = -1;
            std::uint64_t shapeKeyRevision = 0;
            std::vector<ShapeKeyState> shapeKeys;
            // The Mesh's sockets and the skeleton's weights.
            std::string   socketLock, selectedSocket;
            bool          socketsDirty = false;
            std::uint32_t sockets = 0, overlaySockets = 0;
            int           weightsBone = -1;
            bool          weightsLive = false;
            std::size_t   weightParts = 0;
            std::uint64_t weightsRevision = 0;
            std::uint32_t skinBones = 0;   // bones that move this Mesh
        };

        // ---- Lifetime ----
        static MeshEditorWindow& Get();

        // ---- View modes and overlays (Show) ----
        // View modes offered (Renderer::ViewMode values) and their names -
        // "lit", "unlit", "wireframe", "basecolor", "normal", "roughness",
        // "metallic", "ao", "depth".
        static const char* ViewModeName(std::uint32_t mode);
        static bool        ParseViewMode(const std::string& name, std::uint32_t& out);
        // "bounds+pivot+normals", "all", "none" or a hex mask.
        static bool        ParseShow(const std::string& text, std::uint32_t& out);
        static std::string ShowNames(std::uint32_t bits);

        // ---- Lifetime ----
        void Draw(const MeshEditorContext& ctx);
        void ProcessPendingActions(const MeshEditorContext& ctx);   // GAME half, serial window
        void ReleaseViewsAtShutdown(Renderer* renderer);
        bool  IsOpen() const { return m_open; }
        bool* OpenFlag()     { return &m_open; }

        // ---- The subject ----
        // From a UI handler (a render-side context): queued, applied in
        // ProcessPendingActions, through the unsaved-edits question.
        void RequestOpen(const std::string& model, std::uint64_t meshKey, const std::string& meshName);
        // GAME half (Lua, harness): now. @p mesh = a 16-hex MeshKey or a mesh
        // name. Refused ("dirty") while edits are unsaved unless @p discard.
        // @p code: bad_value / load_failed / dirty / unsupported.
        bool OpenNow(const MeshEditorContext& ctx, const std::string& model, const std::string& mesh,
                     bool discard, std::string& code, std::string& why);
        // Close the panel; refused ("dirty") while edits are unsaved unless @p discard.
        bool CloseNow(bool discard, std::string& why);
        // The menu's close: closes, or asks first when there are unsaved edits.
        void RequestClose()
        {
            if (m_loaded && Dirty()) { if (m_askFor == 0) { m_askFor = 1; m_askOpenPopup = true; } }
            else m_open = false;
        }

        // ---- Panel undo ----
        bool OwnsUndoShortcut() const { return m_focused; }
        void RequestUndo() { ++m_pendingUndo; }
        void RequestRedo() { ++m_pendingRedo; }
        void RequestSave() { m_saveQueued = true; }   // the toolbar's Save, run in PPA
        bool Undo();
        bool Redo();

        // ---- Scripted surface (Lua Editor.MeshEditor*, the harness) ----
        bool SetViewMode(std::uint32_t mode);
        void SetShow(std::uint32_t bits) { m_show = bits & kShowAll; }
        // Section visibility: bit i = section i (sections past 63 always drawn).
        bool SetVisibleMask(std::uint64_t mask);
        bool Solo(int section);                // -1 = show all
        bool Select(int section);              // -1 = none; the others draw dimmed
        void SetDimOthers(bool on) { m_dimOthers = on; }
        bool SetUv(int channel, int section);  // section -1 = every visible section
        void SetOrbit(float yaw, float pitch, float distScale);
        // The framed point's shift, in the fitted unit box (ViewDesc::pan).
        void SetPan(float x, float y, float z) { m_pan[0] = x; m_pan[1] = y; m_pan[2] = z; }
        // One undo step. "" = no default material. Refused for a section this
        // model does not own the F-line of.
        bool StageMaterial(int section, const std::string& material, std::string& why);
        bool Revert();                          // staged = saved; one undo step

        bool SaveNow(const MeshEditorContext& ctx, std::string& why, SaveReport* out = nullptr);
        // The model being edited and whether its staged edits are not on disk (the editor's
        // unsaved-documents guard).
        const std::string& DocumentPath() const { return m_model; }
        bool               HasUnsavedEdits() const { return Dirty(); }

        // ---- Collision (see the header comment) ----
        // "box", "sphere", "capsule", "kdop10x", "kdop10y", "kdop10z", "kdop18",
        // "kdop26", "convex".
        static const char* CollisionAddName(CollisionAdd k);
        static bool        ParseCollisionAdd(const std::string& s, CollisionAdd& out);
        // "" = editable; else why not (no mesh, a packed model, a junction, C records,
        // skinned).
        std::string CollisionLock() const;
        // Fit a prim to @p section's vertices (-1 = every section of the Mesh) and
        // add it, selected. One undo step. @p outIndex: its index.
        bool AddCollision(CollisionAdd kind, int section, std::string& why, int* outIndex = nullptr);
        bool DuplicateCollision(int index, std::string& why, int* outIndex = nullptr);   // one undo step
        bool RemoveCollision(int index, std::string& why);   // -1 = every prim; one undo step
        // The whole setup at once - body fields and prim values (not the prim
        // count, order or types, nor a hull's source). One undo step.
        bool StageCollision(const Resource::MeshBodySetup& setup, std::string& why);
        bool SelectCollision(int index);                      // -1 = none; drawn highlighted

        CollisionState Collision();   // GAME half: builds the overlay's shapes first

        // ---- The collision gizmo ----
        // ImGuizmo on the selected prim, in the picture: Move (world axes), Rotate
        // (local), Scale - a box by dragging its faces (ImGuizmo BOUNDS), a sphere /
        // capsule by its handles (a capsule: X / Z = radius, Y = half height); a hull
        // only moves and turns, a trimesh prim has none. One drag = one panel undo
        // step. W / E / R over the picture (never mid-drag).
        static const char* GizmoOpName(GizmoOp op);   // "off", "translate", "rotate", "scale"
        static bool        ParseGizmoOp(const std::string& s, GizmoOp& out);
        void    SetGizmoOp(GizmoOp op) { if (!m_gizmoUsing) m_gizmoOp = op; }
        GizmoOp GetGizmoOp() const { return m_gizmoOp; }
        std::uint32_t GizmoGestures() const { return m_gizmoGestures; }
        // A drag as the gizmo would apply it - the same display-space matrix,
        // the same write-back: Translate by @p v (the Mesh's units), Rotate by
        // @p deg about the prim's local axis @p v, Scale by @p v (factors). One
        // undo step.
        bool GizmoDrag(int index, GizmoOp op, const DirectX::XMFLOAT3& v, float deg, std::string& why);

        // ---- Shape keys (see the header comment) ----
        // The Mesh's targets and their preview state. Weights / solo / heatmap are
        // the picture's, never staged: Lua Editor.MeshEditorShapeKeySet.
        MeshShapeKeys&       ShapeKeys()       { return m_shapeKeys; }
        const MeshShapeKeys& ShapeKeys() const { return m_shapeKeys; }
        // The staged MK / MP records of the Mesh. "" lock = editable (else why not:
        // a packed model, one reached through a junction).
        const std::string&                 ShapeKeyLock() const { return m_shapeKeyLock; }
        const Resource::MeshShapeKeySetup& StagedShapeKeys() const { return m_staged.shapeKeys; }
        // One target's metadata (a default one removes its record). One undo step.
        bool StageShapeKeyMeta(const Resource::ShapeKeyMeta& meta, std::string& why);
        // A preset from what the picture shows now (the counting weights). One undo step.
        bool SaveShapeKeyPreset(const std::string& name, std::string& why);
        bool DeleteShapeKeyPreset(const std::string& name, std::string& why);   // one undo step
        // The picture's weights = the preset's (every other 0, solo off). Not staged.
        bool ApplyShapeKeyPreset(const std::string& name, std::string& why);
        // Every weight to its default (MK default=), solo off. Not staged.
        void ResetShapeKeyWeights();

        // ---- Mesh sockets ----
        // The Mesh's `SO mesh=<key>` records - named attachment points in the Mesh's
        // own space (this picture's): a StaticMesh owner placing the Mesh publishes
        // them (offset x the owner's world). Staged in the panel's undo ring, written
        // by Save (SceneInstanceLoader::ReplaceMeshSockets: this Mesh's records only -
        // the Skeleton Editor's skeleton sockets are never touched here).
        const std::vector<Resource::ModelSocket>& StagedSockets() const { return m_staged.sockets; }
        const std::string& SocketLock() const { return m_socketLock; }   // "" = editable
        bool SocketsDirty() const { return m_staged.sockets != m_saved.sockets; }
        int  FindSocket(const std::string& name) const;                  // -1 = none
        int  SelectedSocket() const { return m_selSocket.empty() ? -1 : FindSocket(m_selSocket); }
        bool SelectSocket(const std::string& name);                      // "" = none
        // Each one undo step; false + @p why = refused, nothing changed.
        // Add: @p name "" = "socket" (made unique); at the Mesh's pivot. @p outName = the name given.
        bool AddSocket(const std::string& name, std::string& outName, std::string& why);
        // Replace the socket named @p name with @p value (a rename when value.name differs).
        bool SetSocket(const std::string& name, const Resource::ModelSocket& value, std::string& why);
        bool RemoveSocket(const std::string& name, std::string& why);

        // ---- The skeleton and its weights ----
        // A skinned Mesh: the bones that move its vertices (the .iskel's blend streams
        // of the F-lines placing its sections) and one bone's weight heatmap on the
        // picture (Editor/BoneWeightHeat - while on, the shape-key preview's copies
        // are not drawn).
        const std::vector<SkinBone>& SkinBones() const { return m_skinBones; }
        int  BoneIndex(const std::string& name) const;   // any bone of the skeleton; -1 = none
        bool SetWeightsBone(int bone);                   // -1 = off; false: no such bone / not skinned
        int  WeightsBone() const { return m_weights.Bone(); }

        // ---- Scripted surface (Lua Editor.MeshEditor*, the harness) ----
        // Everything a script reads (Editor.MeshEditor()).
        StateView State(Renderer* renderer) const;

        // Offline PNGs of the panel's CPU layers (software raster, never a
        // capture of ImGui): the overlay at the view's output size, the UV
        // layout at @p px square. Return lit pixels, -1 on failure (@p why).
        long long WriteOverlayPng(Renderer* renderer, const std::string& path, std::string& why);
        long long WriteUvPng(const std::string& path, std::uint32_t px, std::string& why);

        // ---- Harness (-mesheditor_*) ----
        // Reads every -mesheditor_* flag; false + @p why = a precondition failure
        // (exit 12): a bare or malformed value, or a subject flag with no
        // -mesheditor_open. Called once from App at startup.
        bool HarnessConfigure(std::string& why);
        // True once a harness step could not happen (the subject never
        // resolved, a shot never settled) - EditorLayer turns it into exit 12.
        bool HarnessFailed() const { return m_harnessFailed; }

    private:
        struct Section
        {
            MeshAssetRegistry::Section meta;           // entry, key, slot, default material, aabb
            bool                       editable = false;
            // Decoded from the file: local positions / normals / tangents /
            // uv0..3 / colours / indices.
            std::vector<float>         pos, nrm, tan;  // xyz, xyz, xyzw
            std::vector<float>         uv[4];          // uv, empty = channel absent
            std::vector<std::uint32_t> col;            // RGBA8, empty = absent
            std::vector<std::uint32_t> idx;
            std::uint32_t              vertices = 0, triangles = 0;
        };

        struct StagedCollision
        {
            Resource::MeshBodySetup setup;                       // meshKey = the Mesh's
            // [prim]: a hull's points not in a library yet (Save appends them and
            // fills the prim's lib / sectionKey); empty for every other prim.
            std::vector<std::vector<DirectX::XMFLOAT3>> pending;
        };
        struct Staged
        {
            std::vector<std::string>    materials;               // [section]
            StagedCollision             collision;
            Resource::MeshShapeKeySetup shapeKeys;               // MK / MP (meshKey = the Mesh's)
            std::vector<Resource::ModelSocket> sockets;          // SO mesh=<the Mesh's key>
        };

        static constexpr std::size_t kUndoDepth = 64;

        struct PrimShape
        {
            std::vector<float>         pts;      // xyz
            std::vector<std::uint32_t> lines;    // pairs into pts
        };

        // ---- Harness schedule (editor frames) ----
        struct Step
        {
            std::uint64_t at = 0;
            int           kind = 0;          // see MeshEditorWindow.cpp
            std::string   text;
            std::uint32_t value = 0;
            std::uint64_t mask = 0;
            bool          done = false;
        };

        // ---- Lifetime ----
        MeshEditorWindow() = default;

        // ---- Subject ----
        bool StageShapeKeyMetaImpl(const Resource::ShapeKeyMeta& meta, std::string& why, bool pushUndo);
        void DrawShapeKeyDetails();
        // A target's metadata, staged (its defaults when it has no record).
        Resource::ShapeKeyMeta ShapeKeyMetaOf(const std::string& name) const;

        // Staged defaults and collision + the panel's own history.
        static bool SameCollision(const StagedCollision& a, const StagedCollision& b);
        static bool SameStaged(const Staged& a, const Staged& b)
        {
            return a.materials == b.materials && SameCollision(a.collision, b.collision) &&
                   a.shapeKeys == b.shapeKeys && a.sockets == b.sockets;
        }

        bool Dirty() const { return !SameStaged(m_staged, m_saved); }
        bool CollisionDirty() const { return !SameCollision(m_staged.collision, m_saved.collision); }
        bool ShapeKeysDirty() const { return !(m_staged.shapeKeys == m_saved.shapeKeys); }

        // The prim's matrix in the picture's fitted space (the Mesh's space
        // translated by -centre, scaled by scale): sizes x rotation x centre x fit.
        static void PrimDisplayMatrix(const Resource::CollisionPrim& p, const float centre[3], float scale,
                                      DirectX::XMFLOAT4X4& out);
        // Write a gizmo result back into staged prim @p index (no undo step).
        bool ApplyGizmoMatrix(int index, GizmoOp op, const DirectX::XMFLOAT4X4& display, const float centre[3],
                              float scale);
        void DrawCollisionGizmo(Renderer* renderer, float x, float y, float w, float h);
        void ReleaseGizmo();

        // ---- Mesh sockets and the skeleton's weights ----
        std::string UniqueSocketName(const std::string& base) const;
        bool StageSockets(std::vector<Resource::ModelSocket> next, const char* what, std::string& why,
                          bool pushUndo = true);
        void DrawSocketDetails();

        void DrawSkeletonDetails(const MeshEditorContext& ctx);
        // ---- Subject ----
        void EnsureCollisionShapes();
        void DrawCollisionDetails(const MeshEditorContext& ctx);
        std::vector<DirectX::XMFLOAT3> FitPoints(int section) const;
        // Resource::MeshCountsAsSkinned: an importer-written identity flags the Mesh,
        // or the identity is index-derived (it cannot tell) and the model names a
        // skeleton.
        bool Skinned() const;
        void PushUndo();

        // ---- View state ----
        std::uint64_t VisibleMask() const;
        void          UpdateView(const MeshEditorContext& ctx);

        // ---- Subject ----
        bool LoadSubject(const MeshEditorContext& ctx, const std::string& model, std::uint64_t key,
                         const std::string& name, std::string& code, std::string& why);
        // ---- View state ----
        void ResetView();
        // ---- Drawing ----
        void DrawToolbar(const MeshEditorContext& ctx);
        void DrawViewport(const MeshEditorContext& ctx);
        void DrawUvLayout();
        void DrawDetails(const MeshEditorContext& ctx);
        void DrawStatsLine() const;
        void DrawAskModal();
        // Projected CPU overlays into @p dl over the image rect; returns lines.
        std::uint32_t DrawOverlay(Renderer* renderer, ImDrawList* dl, float x, float y, float w, float h) const;
        // UV triangles into @p dl over a square canvas; fills the m_uv* numbers.
        void DrawUv(ImDrawList* dl, float x, float y, float size) const;

        // ---- Harness schedule (editor frames) ----
        void TickHarness(const MeshEditorContext& ctx);

        // ---- Subject ----
        std::string   m_model;          // project-relative, as given to BuildPreviewModelView
        std::string   m_ref;            // Resource::MeshAssetRef(model, key)
        std::uint64_t m_meshKey = 0;
        std::string   m_meshName;
        std::string   m_libraryPath;
        std::uint32_t m_meshFlags = 0;
        bool          m_meshSkinned = false;    // Resource::MeshCountsAsSkinned (MeshAssetRegistry::Mesh::skinned)
        bool          m_loaded = false;
        bool          m_identityFromFile = false;
        std::string   m_error;
        std::vector<Section> m_sections;
        float         m_boundsMin[3] = { 0.f, 0.f, 0.f }, m_boundsMax[3] = { 0.f, 0.f, 0.f };
        std::uint32_t m_stride = 0, m_libFlags = 0;
        std::uint64_t m_geometryBytes = 0;
        // A skinned Mesh's bind pose: joint positions (xyz, the model's space)
        // and parents, from the model's K-line .iskel.
        std::string        m_skeletonPath;
        std::vector<float> m_joints;
        std::vector<int>   m_jointParent;
        // The Mesh's shape keys and their preview copies.
        MeshShapeKeys      m_shapeKeys;
        char               m_shapeKeyFilter[64] = {};
        std::string        m_shapeKeyLock;               // "" = the MK / MP records are editable
        int                m_shapeKeyCategory = -1;      // the list's filter: -1 all, else a category index
        char               m_presetName[64] = {};
        bool               m_skMetaDragPushed = false;   // a drag in the metadata popup: one undo step

        Staged              m_saved, m_staged;
        std::deque<Staged>  m_undo;
        std::vector<Staged> m_redo;

        int  m_pendingUndo = 0, m_pendingRedo = 0;

        // Collision: why it is read-only ("" = editable), where new hulls go, the
        // selected prim, and what the overlay draws of each staged prim - its
        // points (a hull's, a trimesh's vertices) and lines (index pairs), in the
        // prim's own space; rebuilt when the staged collision changes.
        std::string   m_collisionLock;
        std::string   m_collisionLibrary;
        int           m_collisionSelected = -1;

        std::vector<PrimShape> m_primShapes;     // [prim]
        PrimShape              m_complexShape;   // complexity=complex: every section's triangles
        std::uint64_t          m_primShapesKey = 0;
        mutable std::uint32_t  m_collisionLines = 0;
        // A drag in the Collision section is one undo step: pushed at its first change.
        bool                   m_dragPushed = false;

        // The gizmo: its operation, whether OUR ImGuizmo drag is held / hovered,
        // its undo step, and the matrix fed to ImGuizmo - frozen while hovered or
        // held (re-derived from the staged prim otherwise), in the picture's
        // fitted space.
        GizmoOp             m_gizmoOp = GizmoOp::Translate;
        bool                m_gizmoUsing = false;
        bool                m_gizmoOver = false;
        bool                m_gizmoPushed = false;
        bool                m_orbitActive = false;
        int                 m_gizmoPrim = -1;
        DirectX::XMFLOAT4X4 m_gizmoMatrix{};
        std::uint32_t       m_gizmoGestures = 0;

        // ---- Mesh sockets and the skeleton's weights ----
        std::string   m_socketLock;
        std::string   m_selSocket;                  // by NAME (an edit that shifts indices keeps it)
        bool          m_revealSockets = false;      // open the Sockets section once (a socket was selected)
        bool          m_revealSkeleton = false;     // open the Skeleton section once (a weights bone was set)
        // The Details fields' seeds, keyed by the socket's NAME (an add / remove / revert
        // can put another socket at the same index): the Name box's text, and the
        // Rotation field's degrees - kept while that socket's rotation is the one they
        // were made from (no Euler jitter near gimbal lock), re-seeded when it changed
        // elsewhere (undo, revert, a script).
        char          m_socketNameEdit[64] = {};
        std::string   m_socketNameFor;
        bool          m_socketDragPushed = false;   // a drag in the fields: one undo step
        std::string   m_socketEulerFor;
        DirectX::XMFLOAT4 m_socketEulerQ = { 0.f, 0.f, 0.f, 1.f };
        float         m_socketEuler[3] = { 0.f, 0.f, 0.f };
        mutable std::uint32_t m_overlaySockets = 0;

        BoneWeightHeat            m_weights;        // the picture's heatmap (part k = section k)
        std::vector<SkinBone>     m_skinBones;      // the bones that move this Mesh (vertices > 0)
        std::vector<std::string>  m_boneNames;      // every bone of the skeleton
        char                      m_boneFilter[64] = {};

        // ---- View state ----
        bool          m_open = false;
        bool          m_focused = false;
        std::uint32_t m_viewMode = 0;
        std::uint32_t m_show = kShowBounds | kShowPivot;
        std::vector<bool> m_visible;
        int           m_selected = -1;
        bool          m_dimOthers = true;
        float         m_yaw = 0.f, m_pitch = 0.f, m_dist = 1.f, m_pan[3] = { 0.f, 0.f, 0.f };
        int           m_uvChannel = 0, m_uvSection = -1;
        int           m_tab = 0;                     // 0 viewport, 1 UV layout
        int           m_background = 1;              // Preview::Background (Checker)
        bool          m_shadow = true;

        // The preview view (AssetPreviewWindow's lifecycle).
        std::uint32_t m_viewId = ~0u;
        bool          m_visibleThisFrame = false;
        bool          m_releaseQueued    = false;
        // The size asked for; 0x0 = the picture's box not measured yet this showing, and
        // nothing is asked for until DrawViewport measured it (UpdateView).
        std::uint32_t m_renderW = 0, m_renderH = 0;
        std::uint32_t m_pendW = 0, m_pendH = 0;     // the measured size a resize waits to switch to
        double        m_pendAt = 0.0;
        std::uint32_t m_harnessW = 0, m_harnessH = 0;
        std::uint32_t m_worldEpoch = 0;
        bool          m_worldEpochSeen = false;

        // Queued from the UI (render-side context) for the game half.
        bool          m_openQueued = false;
        std::string   m_queuedModel, m_queuedName;
        std::uint64_t m_queuedKey = 0;
        bool          m_saveQueued = false;
        bool          m_recomposeQueued = false;
        bool          m_closeAfterSave = false;
        // The unsaved-edits question: 0 none, 1 close, 2 open the queued subject.
        int           m_askFor = 0;
        bool          m_askOpenPopup = false;

        // Last UV / overlay numbers (for State and the log).
        mutable int           m_uvLastChannel = 0, m_uvLastSection = -1;
        mutable std::uint32_t m_uvTris = 0, m_uvDrawn = 0;
        mutable float         m_uvMin[2] = { 0.f, 0.f }, m_uvMax[2] = { 0.f, 0.f };
        mutable std::uint32_t m_overlayLines = 0;

        // ---- Harness schedule (editor frames) ----
        std::vector<Step> m_steps;
        std::string   m_harnessOpen;         // "<model>#<key|name>"
        bool          m_harnessOpened = false;
        bool          m_harnessFailed = false;
        bool          m_harnessViewModeOff = false;   // -mesheditor_viewmode_off (reverse arm)
        bool          m_harnessMaskOff = false;       // -mesheditor_mask_off (reverse arm)
        bool          m_harnessUvOff = false;         // -mesheditor_uv_off (reverse arm: positions as UVs)
        std::uint64_t m_rendersAtDesc = 0;             // GetPreviewViewRenderCount when the desc last changed
        std::uint64_t m_lastRenders = 0, m_lastRendersFrame = 0;
        std::string   m_lastDescKey;
    };
}
