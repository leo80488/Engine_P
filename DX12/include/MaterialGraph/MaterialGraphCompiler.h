#pragma once
// MaterialGraph/MaterialGraphCompiler.h - a material graph -> the single-file custom-material hook
// HLSL, exactly what a hand-written custom material provides:
//   #define MATERIAL_HAS_SURFACE
//   #define MATERIAL_HAS_VERTEX_OFFSET      (only when World Position Offset is linked)
//   #include "custom_material.hlsli"
//   cbuffer MaterialGraphParams : register(b8, space0) { P_<Name> ... //@default ... }
//   Texture2D T_<Name> : register(tN, space3); ...
//   void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s) { ... }
//   float3 MaterialGetWorldPositionOffset(MaterialVertexParams p) { ... }
// so the runtime, the reflection-driven parameter UI, instances, packaging and hot reload need nothing
// new. The same graph and options always give the same bytes; every emitted line ends in "// @n<id>"
// so a DXC error line maps back to its node.
//
// EngineCore, no ImGui, no device: Game.exe's -assettool=matgraph runs it in CI.

#include "Graphics/ShadingModel.h"
#include "MaterialGraph/MaterialGraph.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class BlendMode : std::uint32_t;   // ECS/Components.h

namespace MaterialGraph
{
    enum class Severity : std::uint8_t { Info, Warning, Error };

    struct Diagnostic
    {
        std::uint32_t node = 0;       // 0 = the graph as a whole
        std::string   pin;            // empty = the node as a whole
        Severity      severity = Severity::Error;
        std::string   code;           // stable: "cycle", "type_mismatch", "vertex_illegal" ...
        std::string   message;
    };

    struct ParamInfo
    {
        std::string   name;           // the node's props.name ("" for a reserved slot)
        std::string   hlslName;       // P_<Name>, or the reserved name
        std::string   label;          // the @label text (the name; per-component for reserved slots)
        int           components = 1; // 1 (float) or 4 (float4)
        float         def[4] = { 0, 0, 0, 0 };
        bool          hasRange = false;
        float         min[4] = { 0, 0, 0, 0 };
        float         max[4] = { 1, 1, 1, 1 };
        std::string   group;
        std::string   engineRole;     // `@engine <role>` the lighting reads it by; "" = none
        double        sort = 0.0;
        std::uint32_t node = 0;       // declaring node; 0 = reserved by the shading model
        std::uint32_t offset = 0;     // byte offset in the b8 cbuffer
    };

    struct TextureInfo
    {
        std::string   name;           // props.name of a parameter; "" for a fixed TextureSample
        std::string   hlslName;       // T_<Name> / T_Fixed_<sha8>
        std::uint32_t slot = 0;       // space3 register
        std::string   defaultPath;    // the parameter's default / the fixed texture
        std::string   sampler;        // Color | Linear | Normal | Mask | Grayscale
        bool          parameter = false;
        bool          vertex = false; // sampled by World Position Offset
        std::string   group;
        double        sort = 0.0;
        std::vector<std::uint32_t> nodes;   // every node sampling it
    };

    struct CompileOptions
    {
        ShadingModel shadingModel = ShadingModel::Standard;
        BlendMode    blendMode    = static_cast<BlendMode>(0);   // Opaque
        std::string  sourceLabel;     // the graph's path, for the header line
        // Unreal's "Start Previewing Node": the shader shows this node's output pin as emissive and
        // nothing else (no World Position Offset); compile it with the Unlit shading model. 0 = the material
        // itself; previewPin "" = the node's first output. The Material Output itself previews the material.
        std::uint32_t previewNode = 0;
        std::string   previewPin;
        // Static Switch Parameter values by name (a material's or an instance's); one not listed takes
        // its node's props.default.
        std::map<std::string, bool> switches;
    };

    // A Static Switch Parameter the compile met: its name, default and the value it compiled with
    // (options.switches, else the default).
    struct StaticSwitchInfo
    {
        std::string   name;
        bool          def = false;
        bool          value = false;
        std::string   group;
        double        sort = 0.0;
        std::uint32_t node = 0;
    };

    struct CompileStats
    {
        int  nodes = 0;               // in the graph
        int  reachable = 0;           // feed an active output pin
        int  params = 0;              // b8 variables, reserved ones included
        int  cbufferBytes = 0;        // packed, rounded to 16
        int  textures = 0;
        int  hlslBytes = 0;
        int  pixelLines = 0;          // SSA lines in EvaluateMaterial
        int  vertexLines = 0;         // ... in MaterialGetWorldPositionOffset
        bool usesVertex = false;      // World Position Offset is linked
    };

    struct CompileResult
    {
        bool                     ok = false;    // no Error diagnostic
        std::string              hlsl;          // empty when !ok
        std::vector<ParamInfo>   params;        // declaration order
        std::vector<TextureInfo> textures;      // slot order
        std::vector<Diagnostic>  diagnostics;
        CompileStats             stats;
        std::vector<StaticSwitchInfo> switches;         // by name
        std::vector<std::string>      functions;        // the .imatfunc files expanded (sorted)
        // The texture (index into textures) and channel ("R".."A") an active Opacity Mask reads directly,
        // through reroutes; -1 = none. The shadow pass clips by the material's ENGINE slots (OPACITYMAP.r),
        // so the editor can mirror this texture there.
        int                           opacityMaskTexture = -1;
        std::string                   opacityMaskChannel;
        // SHA-256 (16 hex) of what the HLSL depends on: compiler version, options and the nodes' types /
        // props / inputs and links - NOT positions, comments, ownerGuid or nextId (moving a node never
        // recompiles).
        std::string              graphHash;
    };

    // Capacity limits: a graph past one fails to compile (capacity_* errors).
    constexpr int kMaxParams       = 32;    // Resource::kMaxCustomParams (the customParams[] mirror)
    constexpr int kMaxCBufferBytes = 256;   // MaterialCBVRing kSlotStride: past it the persistent store is lost
    constexpr int kMaxTextures     = 8;     // Resource::kMaxCustomTextures; all eight for every shading model

    CompileResult Compile(const Graph& graph, const CompileOptions& options);

    // The compile as JSON - the one shape -assettool=matgraph and Editor.MaterialGraphCompile
    // report: {compiled, graphHash, hlslSha256, errors, warnings, diagnostics [{node, pin?,
    // severity, code, message}], stats, params, textures[, hlsl]}.
    std::string CompileReportJson(const CompileResult& r, bool withHlsl);

    // DXC's errors on the generated HLSL (@p hlsl, compiled as @p sourcePath) as
    // diagnostics on the nodes that emitted the lines ("// @n<id>" ends each one):
    // code "dxc"; an error elsewhere (an include) is the graph's (node 0).
    std::vector<Diagnostic> MapDxcDiagnostics(const std::string& dxcText, const std::string& hlsl,
                                              const std::string& sourcePath);

    // What a generated file was compiled from, read back from its header: the graph (@p graph, the
    // "from" of line 1) and the options (shading model, blend mode, static switches; sourceLabel = the
    // graph). false when @p hlsl is not a material's generated file (another generator's, a node
    // preview's, a switch name the header cannot carry). The editor recompiles a graph's other
    // combinations with it when the graph is saved.
    bool ParseGeneratedHeader(std::string_view hlsl, std::string& graph, CompileOptions& out);

    // Which Material Output pins the shading model / blend mode use. A link into an inactive pin is
    // ignored with a warning.
    bool OutputPinActive(std::string_view pin, ShadingModel model, BlendMode blend);

    const char* SeverityName(Severity s);
    const char* BlendModeLabel(BlendMode b);
    // Accepts the enum's names (case-insensitive) or its integer; false when neither.
    bool ParseShadingModel(std::string_view text, ShadingModel& out);
    bool ParseBlendMode(std::string_view text, BlendMode& out);
}
