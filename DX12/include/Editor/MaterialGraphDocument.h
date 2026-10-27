#pragma once

// MaterialGraphDocument - one material graph (.imatgraph) open for editing: the staged
// MaterialGraph::Graph, its dirty mark, a 64-step undo ring, the compile of the current
// revision, and the clipboard. No ImGui: the Material Editor's canvas
// (Editor/MaterialGraphCanvas) draws it, Lua edits it.
//
// EVERY EDIT IS A BATCH OF OPS (Apply): add / remove / connect / disconnect / set / move /
// comment / uncomment. The canvas and Editor.MaterialGraphEdit speak the same vocabulary,
// so a pack that edits by script proves the canvas's path. A batch is all or nothing and
// one undo step; the batches of one UI gesture (a value dragged over many frames: EndFrame)
// share ONE step. A link into an input replaces the input's link; a link that would close a
// cycle is refused; the Material Output is neither added twice nor removed.
//
// Editor-only TU: compiled into EditorCore.

#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphCompiler.h"
#include "MaterialGraph/MaterialGraphNodes.h"   // NodeDesc (the palette's entries)

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace Editor
{
    struct GraphOp
    {
        enum class Kind : std::uint8_t { Add, Remove, Connect, Disconnect, Set, Move, Comment, SetComment, Uncomment };
        Kind kind = Kind::Add;
        // A node or comment is named by its id, or - inside one batch - by the `as` of the
        // Add or Comment that made it (ref wins when set).
        std::uint32_t node = 0;       std::string nodeRef;      // Remove, Set, Move, SetComment, Uncomment
        std::uint32_t from = 0;       std::string fromRef;      // Connect, Disconnect
        std::uint32_t to = 0;         std::string toRef;
        std::string   fromPin, toPin;
        std::string   type;                                   // Add
        std::string   as;                                     // Add, Comment: the batch-local name of what it makes
        bool          hasPos = false;
        float         pos[2] = { 0, 0 };                      // Add, Move
        MaterialGraph::ValueMap props, inputs;                // Add, Set (merged)
        std::vector<std::string> unsetProps, unsetInputs;     // Set
        bool          hasRect = false, hasColor = false, hasText = false;
        float         rect[4] = { 0, 0, 0, 0 };               // Comment, SetComment
        float         color[4] = { 0.25f, 0.3f, 0.35f, 0.4f };
        std::string   text;
    };

    // Ops from JSON - an array (or {"ops": [...]}) of
    //   {"op": "add", "type": "Multiply", "pos": [x, y], "props": {...}, "inputs": {...}, "as": "m"}
    //   {"op": "remove", "node": id | "name"}
    //   {"op": "connect", "from": id | "name", "fromPin": "Out", "to": id | "name", "toPin": "A"}
    //   {"op": "disconnect", "to": ..., "toPin": "A" [, "from": ..., "fromPin": ...]}
    //   {"op": "set", "node": ..., "props": {...}, "inputs": {...}, "unset": ["k"], "unsetInputs": ["A"]}
    //   {"op": "move", "node": ..., "pos": [x, y]}
    //   {"op": "comment", "rect": [x0, y0, x1, y1], "text": "...", "color": [r, g, b, a], "as": "c"}
    //   {"op": "setcomment", "node": ..., "rect" / "text" / "color"}   {"op": "uncomment", "node": ...}
    // Values are numbers, bools, strings or arrays of numbers. The Lua verb
    // Editor.MaterialGraphEdit and -matgraph_ops both come through here.
    bool ParseGraphOps(const std::string& jsonText, std::vector<GraphOp>& out, std::string& why);

    class MaterialGraphDocument
    {
    public:
        static constexpr std::size_t kUndoDepth = 64;

        // A CompatibleNodes() row: a node type and the pin of the new node the link uses.
        struct PaletteEntry
        {
            const MaterialGraph::NodeDesc* desc = nullptr;
            std::string                    pin;
        };

        // A Parameters() row: the parameter's name, node type and nodes (by id).
        struct ParameterRow
        {
            std::string                name, type;
            std::vector<std::uint32_t> nodes;
            // The node that defines the parameter (lowest id): points into Graph(), invalid once
            // the graph changes (an edit, undo, redo, load).
            const MaterialGraph::Node* first = nullptr;
        };

        // Loads @p path. False with @p why when it does not load; the document is then unchanged.
        bool Open(const std::string& path, std::string& why);
        // A new graph for @p path (not written): a Material Output only.
        void New(const std::string& path, const std::string& ownerGuid);
        // A new Material Function for @p path (not written): Function Input "In" (float3) wired
        // to Function Output "Result".
        void NewFunction(const std::string& path);
        bool IsFunction() const { return m_graph.function; }

        const std::string&           Path() const  { return m_path; }
        const MaterialGraph::Graph&  Graph() const { return m_graph; }
        bool                         Dirty() const { return m_dirty; }
        std::uint64_t                Revision() const { return m_revision; }

        // ---- Editing ----
        // All or nothing: false with @p why (a code, ": ", the reason) changes nothing. @p newIds
        // receives the ids of the Add / Comment ops, in op order.
        bool Apply(const std::vector<GraphOp>& ops, std::vector<std::uint32_t>* newIds, std::string& why);
        // Once per UI frame, after everything that edits it drew: while an item
        // stays active, the batches after the gesture's first share its undo step.
        void EndFrame(bool anyItemActive);

        bool        Undo();
        bool        Redo();
        std::size_t UndoDepth() const { return m_undo.size(); }
        std::size_t RedoDepth() const { return m_redo.size(); }
        // The current graph is the base: nothing to undo (a freshly created graph).
        void        ClearHistory() { m_undo.clear(); m_redo.clear(); }
        // Its material is now @p guid (Save As of the material): not an undo step.
        void        SetOwner(const std::string& guid) { m_graph.ownerGuid = guid; m_dirty = true; ++m_revision; }

        // ---- Clipboard (JSON, the .imatgraph schema: pastes across materials) ----
        // The nodes @p ids (never the Material Output), the links among them and the comments
        // among @p ids.
        std::string CopyText(const std::vector<std::uint32_t>& ids) const;
        // Fresh ids, positions moved by @p offset; one undo step. False with @p why: not a graph,
        // or nothing to paste.
        bool Paste(const std::string& text, const float offset[2], std::vector<std::uint32_t>* newIds, std::string& why);

        // ---- Compile ----
        // The current revision compiled with @p o (cached per revision + options).
        const MaterialGraph::CompileResult& Compiled(const MaterialGraph::CompileOptions& o);

        // ---- Files ----
        bool Save(std::string& why);           // atomic; clears Dirty
        bool SaveAs(const std::string& path, std::string& why);
        bool Revert(std::string& why);         // one undo step
        // The file changed on disk since it was read or saved here.
        bool ChangedOnDisk() const;

        // The Material Output's id (0 when the graph has none).
        std::uint32_t OutputId() const;

        // ---- Find, Hide Unrelated ----
        // The nodes whose label, type, text props (a parameter's or reroute's name, a texture, a
        // Custom node's code) or "#<id>" contain @p text, ignoring case; by id.
        std::vector<std::uint32_t> Find(const std::string& text) const;
        // Every node upstream or downstream of @p seeds (the seeds too) through links and
        // Named Reroutes; by id.
        std::vector<std::uint32_t> Related(const std::vector<std::uint32_t>& seeds) const;

        // ---- The type-filtered palette, the Parameters panel ----
        // The node types a wire dragged from a pin into empty space may become, each with the new
        // node's pin that link uses (its first compatible one). The dragged pin carries
        // @p components values (0 = sized when compiled: anything fits) and is an output
        // (@p fromOutput: the new node takes it on an input) or an input. The canvas's drag rule
        // decides: a vector never widens (float2 into float3), a scalar broadcasts, a larger value
        // truncates. Material Output and Material Function Call (its pins come from its function,
        // unknown until set) never appear.
        static std::vector<PaletteEntry> CompatibleNodes(int components, bool fromOutput);

        // Every parameter of the graph, one row per NAME and kind (same-named nodes are one
        // parameter): Scalar / Vector / Texture Sample Parameters and Static Switch Parameters,
        // ordered by (switches last, group, sort, name).
        std::vector<ParameterRow> Parameters() const;
        // Sets @p props on EVERY node of parameter @p name (so they stay one parameter): one undo
        // step. @p name picks the first row of that name Parameters() lists (a value parameter
        // before a same-named switch). props.name renames it, refused when another parameter of
        // its namespace (value parameters, or switches) has that name (param_name_duplicate).
        // False with @p why: not_found, bad_value, or the refusal.
        bool SetParameter(const std::string& name, const MaterialGraph::ValueMap& props, std::string& why);

    private:
        struct CompileSlot
        {
            MaterialGraph::CompileResult result;
            std::uint64_t                revision = 0;
            int                          shading = -1, blend = -1;
            std::string                  label;
            std::uint32_t                previewNode = 0;
            std::string                  previewPin;
            std::map<std::string, bool>  switches;
            std::uint64_t                epoch = 0;    // MaterialGraph::FunctionEpoch() it saw
            std::uint64_t                used = 0;
        };

        bool ApplyOne(MaterialGraph::Graph& g, const GraphOp& op, std::map<std::string, std::uint32_t>& names,
                      std::vector<std::uint32_t>* newIds, std::string& why) const;
        void Commit(MaterialGraph::Graph&& next);
        void NoteDiskStamp();

        std::string                      m_path;
        MaterialGraph::Graph             m_graph;
        std::string                      m_savedText;          // the file as read / last written: Dirty compares
        bool                             m_dirty = false;
        std::uint64_t                    m_revision = 1;
        std::deque<MaterialGraph::Graph> m_undo, m_redo;
        bool                             m_mergeNext = false;          // a gesture's later batches: no new step
        bool                             m_committedThisFrame = false;
        std::uint64_t                    m_diskSize = 0;
        std::int64_t                     m_diskTime = 0;

        // Two compiles kept: the material's and a previewed node's are both read every frame
        // while a node is previewed, and one slot would recompile both.
        CompileSlot                      m_compiled[2];
        std::uint64_t                    m_compileTick = 0;
    };
}
