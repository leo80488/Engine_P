#pragma once
// MaterialGraph/MaterialGraph.h - the .imatgraph document: nodes, links and comments of one
// material graph. A graph is the authoring layer of a custom-shader material; the compiler
// (MaterialGraphCompiler.h) turns it into single-file hook HLSL and the runtime never reads a graph.
//
// The file is JSON written deterministically (SerializeGraph): keys sorted, nodes by id, links by
// their input, one node / link / comment per line, every number in its shortest round-trip spelling,
// so an edit is a clean diff and the same graph is always the same bytes.
//
// Node ids only increase (nextId) and are never reused, so the identifiers the compiler derives from
// them (n<id>_<pin>) stay stable.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace MaterialGraph
{
    constexpr int  kFormatVersion     = 1;   // the file's "version"
    constexpr int  kCompilerVersion   = 1;   // the codegen generation (header "MaterialGraph v<N>")
    constexpr char kFormatTag[]       = "imatgraph";
    constexpr char kFunctionFormatTag[] = "imatfunc";   // a Material Function: the same schema

    // A property or an unconnected input's value: a JSON number, bool, string
    // or array of numbers - everything a node stores.
    struct Value
    {
        enum class Kind : std::uint8_t { None, Number, Bool, String, Numbers };

        Kind                kind = Kind::None;
        double              num  = 0.0;
        bool                b    = false;
        std::string         str;
        std::vector<double> nums;

        static Value Number(double v);
        static Value Bool(bool v);
        static Value String(std::string v);
        static Value Numbers(std::vector<double> v);

        // Numeric view: Number = 1 component, Numbers = its size, Bool = 1 (0/1).
        bool   IsNumeric() const { return kind == Kind::Number || kind == Kind::Numbers || kind == Kind::Bool; }
        int    Components() const;
        double At(int i) const;          // 0 past the end

        bool operator==(const Value& o) const;
        bool operator!=(const Value& o) const { return !(*this == o); }
    };
    using ValueMap = std::map<std::string, Value>;   // sorted: deterministic

    struct Node
    {
        std::uint32_t id = 0;
        std::string   type;
        float         pos[2] = { 0.0f, 0.0f };
        ValueMap      props;    // node settings (parameter name, texture path, mask ...)
        ValueMap      inputs;   // values of UNCONNECTED input pins, by pin name

        const Value* Prop(std::string_view key) const;
        double       PropNumber(std::string_view key, double fallback) const;
        std::string  PropString(std::string_view key, const std::string& fallback = {}) const;
        bool         PropBool(std::string_view key, bool fallback) const;
    };

    // fromNode.fromPin (an output) -> toNode.toPin (an input). An input has at
    // most one link; an output any number.
    struct Link
    {
        std::uint32_t fromNode = 0;
        std::string   fromPin;
        std::uint32_t toNode = 0;
        std::string   toPin;

        bool operator==(const Link& o) const
        {
            return fromNode == o.fromNode && fromPin == o.fromPin && toNode == o.toNode && toPin == o.toPin;
        }
    };

    struct Comment
    {
        std::uint32_t id = 0;
        float         rect[4]  = { 0, 0, 0, 0 };   // x0 y0 x1 y1, canvas units
        std::string   text;
        float         color[4] = { 0, 0, 0, 0 };
    };

    struct Graph
    {
        int                  version  = kFormatVersion;
        int                  compiler = kCompilerVersion;
        // A Material Function (.imatfunc, "format": "imatfunc"): Function Input / Output nodes instead
        // of a Material Output; a graph calls it (Material Function Call) and the compiler expands it in place.
        bool                 function = false;
        std::string          ownerGuid;       // the .imat this graph belongs to
        std::uint32_t        nextId   = 1;    // next node / comment id; never reused
        std::vector<Node>    nodes;
        std::vector<Link>    links;
        std::vector<Comment> comments;

        Node*       FindNode(std::uint32_t id);
        const Node* FindNode(std::uint32_t id) const;
        // The link feeding toNode.toPin, or nullptr.
        const Link* FindInputLink(std::uint32_t toNode, std::string_view toPin) const;
        std::uint32_t AllocateId() { return nextId++; }
    };

    // Parse .imatgraph text. Refuses: not JSON, format != "imatgraph", a version
    // newer than kFormatVersion, duplicate or zero ids, malformed nodes / links /
    // values. nextId is raised past the largest id it finds (never lowered).
    bool ParseGraph(std::string_view text, Graph& out, std::string& error);

    // The deterministic text (see the file comment). Always ends in '\n'.
    std::string SerializeGraph(const Graph& g);

    bool LoadGraph(const std::string& utf8Path, Graph& out, std::string& error);
    // Atomic (Sys::WriteFileBytesAtomic): a reader sees the old file or the new one.
    bool SaveGraph(const std::string& utf8Path, const Graph& g, std::string& error);

    // A Material Function by path, parsed and kept while its file is unchanged (size / time checked at
    // most every 250 ms): the compiler expands it, the node registry reads a call's pins from it.
    // nullptr + error when it cannot be read or parsed. Thread-safe.
    std::shared_ptr<const Graph> LoadFunctionCached(const std::string& utf8Path, std::string& error);
    // Moves whenever a cached function is re-read (its file changed): a compile kept across frames
    // compares it, so a function saved anywhere recompiles its callers.
    std::uint64_t FunctionEpoch();
    // Every function @p g calls directly (Material Function Call props.function), checked
    // against its file now (a cached compile's staleness test).
    void TouchCalledFunctions(const Graph& g);

    // Shortest text that reads back as the same float ("0.8", "600", "1e-08").
    // Integral values print without a fraction. Used by the file and the compiler.
    std::string FormatNumber(double v);
}
