#pragma once
// MaterialGraph/MaterialGraphNodes.h - the node types a material graph may use: their pins, the
// defaults of unconnected inputs, and which stages they may run in. The compiler
// (MaterialGraphCompiler.cpp) owns what each node MEANS; this table is what an editor needs to draw a
// node and to check a connection.
//
// Pin component counts: 1..4 = that many floats; 0 = "follows its inputs" (Add of two float3 is a
// float3). An input with a fixed count accepts a smaller source only when it is a scalar (broadcast),
// and a larger one always (truncated) - the rules of cast() in
// tools/cartoonstreet_migration/material_graph.py.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace MaterialGraph
{
    struct PinDesc
    {
        const char* name       = "";        // the key links and a node's "inputs" use
        const char* label      = "";        // UI text
        int         components = 1;         // 1..4, or 0 = dynamic
        float       def[4]     = { 0, 0, 0, 0 };
        int         defCount   = 1;         // how many of def[] an unconnected input uses
        // Unconnected and no value stored: this HLSL instead of def[] (the
        // stage's built-in - TextureCoordinate's UVs default to p.uv0).
        const char* implicitPS = nullptr;
        const char* implicitVS = nullptr;
        // Not drawn and not linkable by hand: the compiler links it (a Named Reroute Usage's input is
        // its declaration's value).
        bool        hidden = false;
    };

    // A node setting (Node::props). The compiler reads props by name; this is what an editor offers
    // for them (the node panel) and their defaults.
    struct PropDesc
    {
        enum class Kind : std::uint8_t { Number, Numbers, Bool, String, Path, Choice, Code };   // Code: multi-line text
        const char* name  = "";
        Kind        kind  = Kind::Number;
        int         count = 1;           // Numbers: how many
        float       def[4] = { 0, 0, 0, 0 };
        bool        defBool = false;
        const char* defText = "";        // String / Path / Choice
        const char* choices = "";        // Choice: '|'-separated
        bool        color   = false;     // Numbers: edit as a colour
    };

    enum NodeFlags : std::uint32_t
    {
        kNodePixelOnly = 1u << 0,   // an error when World Position Offset reaches it
        kNodeParameter = 1u << 1,   // declares a b8 parameter (props.name)
        kNodeTexture   = 1u << 2,   // takes a space3 texture slot
        kNodeOutput    = 1u << 3,   // the Material Output node
        kNodeChannels  = 1u << 4,   // outputs RGB/R/G/B/A/RGBA of one float4 value
        kNodeStatic    = 1u << 5,   // a compile-time parameter (props.name; no b8 variable)
        kNodeFunction  = 1u << 6,   // a Material Function's own node (Function Input / Output)
    };

    struct NodeDesc
    {
        const char*          type     = "";
        const char*          category = "";
        const char*          label    = "";
        const char*          help     = "";
        std::vector<PinDesc> inputs;
        std::vector<PinDesc> outputs;
        std::uint32_t        flags    = 0;
        std::vector<PropDesc> props;

        const PinDesc* Input(std::string_view name) const;
        const PinDesc* Output(std::string_view name) const;
        int            InputIndex(std::string_view name) const;    // -1 when absent
        int            OutputIndex(std::string_view name) const;
    };

    // Every node type, in palette order: grouped by category, a fixed order inside each.
    const std::vector<NodeDesc>& NodeRegistry();
    const NodeDesc*              FindNodeDesc(std::string_view type);

    struct Node;
    // A node's pins: its type's (FindNodeDesc) - except a Custom node, whose pins
    // are what its props declare (inputs, outputType). Such a description is built
    // once per distinct declaration and kept, so the pointer is as stable as the
    // registry's. Use this, not FindNodeDesc, wherever a NODE's pins matter.
    const NodeDesc* DescribeNode(const Node& n);

    // The Custom node: props.inputs declares its input pins, "A:float3 B:float" (whitespace or commas
    // between; float, float2, float3, float4), props.outputType its one output (float .. float4). Parse
    // reports the first bad entry in `error` and keeps the good ones.
    struct CustomInput
    {
        std::string name;
        int         components = 1;
    };
    constexpr int  kMaxCustomInputs      = 8;
    constexpr char kCustomDefaultInputs[] = "A:float3";   // the props' defaults (the registry entry's pins)
    constexpr char kCustomDefaultOutput[] = "float3";
    constexpr char kCustomDefaultCode[]   = "return A;";
    bool ParseCustomInputs(std::string_view spec, std::vector<CustomInput>& out, std::string* error);
    int  CustomTypeComponents(std::string_view type);   // "float2" -> 2; 0 when not a type

    // A Material Function's interface: its Function Inputs / Outputs ordered by props.sort, then name
    // - a call's input / output pins, in that order. An input's components are its props.type's; an
    // output's follow its value (0).
    struct FunctionPin
    {
        std::string   name;
        int           components = 0;
        std::uint32_t node = 0;
        double        sort = 0.0;
    };
    struct Graph;
    void FunctionPins(const Graph& fn, std::vector<FunctionPin>& inputs, std::vector<FunctionPin>& outputs);
}
