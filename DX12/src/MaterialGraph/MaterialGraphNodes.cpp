#include "MaterialGraph/MaterialGraphNodes.h"

#include "MaterialGraph/MaterialGraph.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace MaterialGraph
{
namespace
{
    // An input with a stored default (def[] / defCount).
    PinDesc In(const char* name, int components, std::initializer_list<float> def, const char* label = nullptr)
    {
        PinDesc p;
        p.name       = name;
        p.label      = label ? label : name;
        p.components = components;
        p.defCount   = 0;
        for (float v : def)
            if (p.defCount < 4) p.def[p.defCount++] = v;
        if (p.defCount == 0) p.defCount = 1;
        return p;
    }

    // An input whose unconnected value is a stage built-in (p.uv0, p.time ...).
    PinDesc Implicit(const char* name, int components, const char* ps, const char* vs, const char* label = nullptr)
    {
        PinDesc p;
        p.name       = name;
        p.label      = label ? label : name;
        p.components = components;
        p.defCount   = 0;
        p.implicitPS = ps;
        p.implicitVS = vs;
        return p;
    }

    // A Material Output pin: unconnected means "keep the engine's default".
    PinDesc Sink(const char* name, int components, const char* label)
    {
        PinDesc p;
        p.name       = name;
        p.label      = label;
        p.components = components;
        p.defCount   = 0;
        return p;
    }

    PinDesc Out(const char* name, int components, const char* label = nullptr)
    {
        PinDesc p;
        p.name       = name;
        p.label      = label ? label : name;
        p.components = components;
        p.defCount   = 0;
        return p;
    }

    std::vector<PinDesc> ChannelOutputs()
    {
        return { Out("RGB", 3), Out("R", 1), Out("G", 1), Out("B", 1), Out("A", 1), Out("RGBA", 4) };
    }

    NodeDesc Make(const char* type, const char* category, const char* label, const char* help,
                  std::vector<PinDesc> inputs, std::vector<PinDesc> outputs, std::uint32_t flags = 0)
    {
        NodeDesc d;
        d.type     = type;
        d.category = category;
        d.label    = label;
        d.help     = help;
        d.inputs   = std::move(inputs);
        d.outputs  = std::move(outputs);
        d.flags    = flags;
        return d;
    }

    PropDesc PNum(const char* name, float def)
    {
        PropDesc p;
        p.name   = name;
        p.kind   = PropDesc::Kind::Number;
        p.def[0] = def;
        return p;
    }

    PropDesc PNums(const char* name, std::initializer_list<float> def, bool color = false)
    {
        PropDesc p;
        p.name  = name;
        p.kind  = PropDesc::Kind::Numbers;
        p.count = 0;
        for (float v : def)
            if (p.count < 4) p.def[p.count++] = v;
        p.color = color;
        return p;
    }

    PropDesc PBool(const char* name, bool def)
    {
        PropDesc p;
        p.name    = name;
        p.kind    = PropDesc::Kind::Bool;
        p.defBool = def;
        return p;
    }

    PropDesc PText(const char* name, const char* def, PropDesc::Kind kind = PropDesc::Kind::String,
                   const char* choices = "")
    {
        PropDesc p;
        p.name    = name;
        p.kind    = kind;
        p.defText = def;
        p.choices = choices;
        return p;
    }

    // What each type's props are (the compiler reads them by these names).
    void AttachProps(std::vector<NodeDesc>& r)
    {
        const char* kSamplers = "Color|Linear|Normal|Mask|Grayscale";
        const char* kSpaces   = "Tangent|World|Local";
        auto param = [&](std::vector<PropDesc> extra) {
            std::vector<PropDesc> v{ PText("name", "Param") };
            v.insert(v.end(), extra.begin(), extra.end());
            v.push_back(PText("group", ""));
            v.push_back(PNum("sort", 0.0f));
            return v;
        };
        for (NodeDesc& d : r)
        {
            const std::string t = d.type;
            if      (t == "MaterialOutput")           d.props = { PBool("tangentSpaceNormal", true) };
            else if (t == "Constant")                 d.props = { PNum("value", 0.0f) };
            else if (t == "Constant2Vector")          d.props = { PNums("value", { 0, 0 }) };
            else if (t == "Constant3Vector")          d.props = { PNums("value", { 0, 0, 0 }, true) };
            else if (t == "Constant4Vector")          d.props = { PNums("value", { 0, 0, 0, 1 }, true) };
            else if (t == "ScalarParameter")          d.props = param({ PNum("default", 0.0f), PNum("min", 0.0f), PNum("max", 1.0f) });
            else if (t == "VectorParameter")          d.props = param({ PNums("default", { 1, 1, 1, 1 }, true) });
            else if (t == "TextureSampleParameter2D") d.props = param({ PText("default", "", PropDesc::Kind::Path),
                                                                        PText("sampler", "Color", PropDesc::Kind::Choice, kSamplers) });
            else if (t == "TextureSample")            d.props = { PText("texture", "", PropDesc::Kind::Path),
                                                                  PText("sampler", "Color", PropDesc::Kind::Choice, kSamplers) };
            else if (t == "TextureCoordinate")        d.props = { PNum("index", 0.0f), PNum("uTiling", 1.0f), PNum("vTiling", 1.0f) };
            else if (t == "BumpOffset")               d.props = { PNum("referencePlane", 0.5f) };
            else if (t == "ComponentMask")            d.props = { PBool("r", true), PBool("g", false), PBool("b", false), PBool("a", false) };
            else if (t == "Time")                     d.props = { PNum("period", 0.0f) };
            else if (t == "Transform")                d.props = { PText("source", "Tangent", PropDesc::Kind::Choice, kSpaces),
                                                                  PText("dest", "World", PropDesc::Kind::Choice, kSpaces) };
            else if (t == "Desaturation")             d.props = { PNums("luminanceFactors", { 0.3f, 0.59f, 0.11f }) };
            else if (t == "Sine" || t == "Cosine")    d.props = { PNum("period", 1.0f) };
            else if (t == "If")                       d.props = { PNum("threshold", 0.00001f) };
            else if (t == "Rotator")                  d.props = { PNum("centerX", 0.5f), PNum("centerY", 0.5f), PNum("speed", 0.25f) };
            else if (t == "NamedRerouteDeclaration")  d.props = { PText("name", "Reroute"), PNums("color", { 0.25f, 0.55f, 0.95f }, true) };
            else if (t == "NamedRerouteUsage")        d.props = { PText("name", "Reroute") };
            else if (t == "Custom")                   d.props = { PText("description", ""), PText("inputs", kCustomDefaultInputs),
                                                                  PText("outputType", kCustomDefaultOutput, PropDesc::Kind::Choice,
                                                                        "float|float2|float3|float4"),
                                                                  PText("code", kCustomDefaultCode, PropDesc::Kind::Code) };
            else if (t == "SceneTexture")             d.props = { PText("sceneTexture", "SceneDepth", PropDesc::Kind::Choice,
                                                                        "SceneDepth|PostProcessInput0|CustomDepth|WorldNormal") };
            else if (t == "StaticBool")               d.props = { PBool("value", true) };
            else if (t == "StaticSwitchParameter")    d.props = { PText("name", "Switch"), PBool("default", true),
                                                                  PText("group", ""), PNum("sort", 0.0f) };
            else if (t == "StaticSwitch")             d.props = { PBool("default", true) };
            else if (t == "FunctionInput")            d.props = { PText("name", "In"),
                                                                  PText("type", "float3", PropDesc::Kind::Choice,
                                                                        "float|float2|float3|float4"),
                                                                  PNum("sort", 0.0f), PText("description", "") };
            else if (t == "FunctionOutput")           d.props = { PText("name", "Result"), PNum("sort", 0.0f),
                                                                  PText("description", "") };
            else if (t == "MaterialFunctionCall")     d.props = { PText("function", "", PropDesc::Kind::Path) };
        }
    }

    std::vector<NodeDesc> Build()
    {
        std::vector<NodeDesc> r;
        const char* kConst  = "Constants";
        const char* kParam  = "Parameters";
        const char* kTex    = "Texture";
        const char* kMath   = "Math";
        const char* kVector = "Vector";
        const char* kWorld  = "World";
        const char* kUtil   = "Utility";
        const char* kEngine = "Engine";

        // ---- Output ----
        r.push_back(Make("MaterialOutput", "Output", "Material Output",
            "The material's surface. Which pins are active follows the .imat's shading model and "
            "blend mode; an unconnected pin keeps the engine default.",
            { Sink("BaseColor", 3, "Base Color"), Sink("Metallic", 1, "Metallic"),
              Sink("Specular", 1, "Specular"), Sink("Roughness", 1, "Roughness"),
              Sink("Anisotropy", 1, "Anisotropy"), Sink("EmissiveColor", 3, "Emissive Color"),
              Sink("Opacity", 1, "Opacity"), Sink("OpacityMask", 1, "Opacity Mask"),
              Sink("Normal", 3, "Normal"), Sink("Tangent", 3, "Tangent"),
              Sink("WorldPositionOffset", 3, "World Position Offset"),
              Sink("SubsurfaceOpacity", 1, "Subsurface Opacity"), Sink("SubsurfaceMask", 1, "Subsurface Mask"),
              Sink("ClearCoat", 1, "Clear Coat"), Sink("ClearCoatRoughness", 1, "Clear Coat Roughness"),
              Sink("AmbientOcclusion", 1, "Ambient Occlusion"), Sink("DirectAdditive", 1, "Direct Additive") },
            {}, kNodeOutput));

        // ---- Constants ----
        r.push_back(Make("Constant", kConst, "Constant", "props.value: a float.",
            {}, { Out("Out", 1) }));
        r.push_back(Make("Constant2Vector", kConst, "Constant 2 Vector", "props.value: [x, y].",
            {}, { Out("Out", 2), Out("R", 1), Out("G", 1) }));
        r.push_back(Make("Constant3Vector", kConst, "Constant 3 Vector", "props.value: [r, g, b].",
            {}, { Out("Out", 3), Out("R", 1), Out("G", 1), Out("B", 1) }));
        r.push_back(Make("Constant4Vector", kConst, "Constant 4 Vector", "props.value: [r, g, b, a].",
            {}, { Out("Out", 4), Out("R", 1), Out("G", 1), Out("B", 1), Out("A", 1) }));

        // ---- Parameters ----
        r.push_back(Make("ScalarParameter", kParam, "Scalar Parameter",
            "A b8 float P_<name>. props: name, default, min, max, group, sort. The material's value "
            "is its cparam_ key; changing it never recompiles.",
            {}, { Out("Out", 1) }, kNodeParameter));
        r.push_back(Make("VectorParameter", kParam, "Vector Parameter",
            "A b8 float4 P_<name>. props: name, default [4], min, max, group, sort.",
            {}, ChannelOutputs(), kNodeParameter | kNodeChannels));
        r.push_back(Make("TextureSampleParameter2D", kParam, "Texture Sample Parameter 2D",
            "A space3 texture T_<name>, sampled. props: name, default (.itex path), sampler "
            "(Color|Linear|Normal|Mask|Grayscale), group, sort.",
            { Implicit("UVs", 2, "p.uv0", "p.uv0") }, ChannelOutputs(),
            kNodeParameter | kNodeTexture | kNodeChannels));
        // Compile-time: the branch not taken is not compiled (nor its textures and parameters).
        r.push_back(Make("StaticBool", kParam, "Static Bool",
            "A compile-time true / false (props.value): a Static Switch's Value. Read as a number it is 1 or 0.",
            {}, { Out("Out", 1) }));
        r.push_back(Make("StaticSwitchParameter", kParam, "Static Switch Parameter",
            "A compile-time switch (props.name, props.default, group, sort): True or False is compiled - never both. "
            "A material or an instance picks the value; each combination is its own shader.",
            { In("True", 0, { 1.0f }), In("False", 0, { 0.0f }) }, { Out("Out", 0) }, kNodeStatic));
        r.push_back(Make("StaticSwitch", kParam, "Static Switch",
            "True or False, chosen when compiled by Value - a Static Bool (through reroutes and function inputs); "
            "unconnected, props.default.",
            { In("True", 0, { 1.0f }), In("False", 0, { 0.0f }), Sink("Value", 1, "Value") }, { Out("Out", 0) }));

        // ---- Texture and UV ----
        r.push_back(Make("TextureSample", kTex, "Texture Sample",
            "A fixed texture (props.texture, an .itex path) in its own space3 slot T_Fixed_<sha8>; "
            "not overridable by an instance. props.sampler as the parameter.",
            { Implicit("UVs", 2, "p.uv0", "p.uv0") }, ChannelOutputs(), kNodeTexture | kNodeChannels));
        r.push_back(Make("TextureCoordinate", kTex, "Texture Coordinate",
            "p.uv<index> * (uTiling, vTiling). props: index 0..3, uTiling, vTiling.",
            {}, { Out("Out", 2) }));
        r.push_back(Make("Panner", kTex, "Panner", "Coordinate + Time * Speed.",
            { Implicit("Coordinate", 2, "p.uv0", "p.uv0"), Implicit("Time", 1, "p.time", "p.time"),
              In("Speed", 2, { 0.0f, 0.0f }) },
            { Out("Out", 2) }));
        r.push_back(Make("BumpOffset", kTex, "Bump Offset",
            "Parallax: Coordinate + viewTS.xy * (Height - referencePlane) * HeightRatio. "
            "props.referencePlane (0.5). Pixel stage only.",
            { Implicit("Coordinate", 2, "p.uv0", "p.uv0"), In("Height", 1, { 0.0f }),
              In("HeightRatio", 1, { 0.05f }, "Height Ratio") },
            { Out("Out", 2) }, kNodePixelOnly));
        r.push_back(Make("Rotator", kTex, "Rotator",
            "Coordinate rotated about (centerX, centerY) by Time * speed radians (Unreal's). props: centerX, centerY "
            "(0.5), speed (0.25).",
            { Implicit("Coordinate", 2, "p.uv0", "p.uv0"), Implicit("Time", 1, "p.time", "p.time") },
            { Out("Out", 2) }));

        // ---- Math ----
        r.push_back(Make("Add", kMath, "Add", "A + B.", { In("A", 0, { 0.0f }), In("B", 0, { 1.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Subtract", kMath, "Subtract", "A - B.", { In("A", 0, { 1.0f }), In("B", 0, { 1.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Multiply", kMath, "Multiply", "A * B.", { In("A", 0, { 0.0f }), In("B", 0, { 1.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Divide", kMath, "Divide", "A / B.", { In("A", 0, { 1.0f }), In("B", 0, { 2.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("OneMinus", kMath, "1-x", "1 - Input.", { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Abs", kMath, "Abs", "abs(Input).", { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Ceil", kMath, "Ceil", "ceil(Input).", { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Frac", kMath, "Frac", "frac(Input).", { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Saturate", kMath, "Saturate", "saturate(Input).", { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Clamp", kMath, "Clamp", "clamp(Input, Min, Max).",
            { In("Input", 0, { 0.0f }), In("Min", 0, { 0.0f }), In("Max", 0, { 1.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Min", kMath, "Min", "min(A, B).", { In("A", 0, { 0.0f }), In("B", 0, { 1.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Max", kMath, "Max", "max(A, B).", { In("A", 0, { 0.0f }), In("B", 0, { 1.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Power", kMath, "Power", "pow(max(abs(Base), 1e-8), Exp).",
            { In("Base", 0, { 0.0f }), In("Exp", 0, { 2.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("LinearInterpolate", kMath, "Lerp", "lerp(A, B, Alpha).",
            { In("A", 0, { 0.0f }), In("B", 0, { 1.0f }), In("Alpha", 0, { 0.5f }) }, { Out("Out", 0) }));
        r.push_back(Make("DotProduct", kMath, "Dot", "dot(A, B).",
            { In("A", 0, { 0.0f }), In("B", 0, { 0.0f }) }, { Out("Out", 1) }));
        r.push_back(Make("CrossProduct", kMath, "Cross", "cross(A, B).",
            { In("A", 3, { 0.0f, 0.0f, 1.0f }), In("B", 3, { 1.0f, 0.0f, 0.0f }) }, { Out("Out", 3) }));
        r.push_back(Make("Normalize", kMath, "Normalize",
            "Input / max(length(Input), 1e-5): a zero vector gives 0, never NaN.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Distance", kMath, "Distance", "distance(A, B).",
            { In("A", 0, { 0.0f }), In("B", 0, { 0.0f }) }, { Out("Out", 1) }));
        r.push_back(Make("DDX", kMath, "DDX", "ddx(Input). Pixel stage only.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }, kNodePixelOnly));
        r.push_back(Make("DDY", kMath, "DDY", "ddy(Input). Pixel stage only.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }, kNodePixelOnly));
        r.push_back(Make("Floor", kMath, "Floor", "floor(Input).", { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Sqrt", kMath, "Sqrt", "sqrt(max(Input, 0)): a negative input gives 0, never NaN.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Length", kMath, "Length", "length(Input); a scalar's is abs(Input).",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 1) }));
        r.push_back(Make("Sine", kMath, "Sine",
            "sin(Input * 2pi / period): props.period (1, as Unreal) is one full wave; period <= 0: sin(Input) in radians.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("Cosine", kMath, "Cosine",
            "cos(Input * 2pi / period): props.period (1, as Unreal) is one full wave; period <= 0: cos(Input) in radians.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("If", kMath, "If",
            "Unreal's If: |A - B| <= props.threshold (1e-5) gives A == B, else A >= B gives A > B, else A < B. With "
            "A == B unconnected: A >= B ? A > B : A < B. A and B are scalars; the results must match or be scalars.",
            { In("A", 1, { 0.0f }), In("B", 1, { 0.0f }), In("AGreaterThanB", 0, { 1.0f }, "A > B"),
              Sink("AEqualsB", 0, "A == B"), In("ALessThanB", 0, { 0.0f }, "A < B") },
            { Out("Out", 0) }));

        // ---- Vector ----
        r.push_back(Make("ComponentMask", kVector, "Component Mask", "props r, g, b, a (bools): the kept channels.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("AppendVector", kVector, "Append", "float(A..., B...), at most 4 components.",
            { In("A", 0, { 0.0f }), In("B", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("BreakOutFloat4Components", kVector, "Break Out Float4",
            "The four components of a float4 (a scalar broadcasts; a float2 / float3 is a type error, as Unreal's).",
            { In("Float4", 4, { 0.0f, 0.0f, 0.0f, 0.0f }) },
            { Out("R", 1), Out("G", 1), Out("B", 1), Out("A", 1) }, kNodeChannels));

        // ---- World and geometry ----
        r.push_back(Make("WorldPosition", kWorld, "World Position", "The world position (metres), before World Position Offset.",
            {}, { Out("Out", 3) }));
        r.push_back(Make("ObjectPositionWS", kWorld, "Object Position", "The object's world origin.",
            {}, { Out("Out", 3) }));
        r.push_back(Make("CameraPositionWS", kWorld, "Camera Position", "The camera's world position.",
            {}, { Out("Out", 3) }));
        r.push_back(Make("CameraVectorWS", kWorld, "Camera Vector", "Unit vector from the surface to the camera.",
            {}, { Out("Out", 3) }));
        r.push_back(Make("VertexNormalWS", kWorld, "Vertex Normal", "The interpolated geometric normal, world space.",
            {}, { Out("Out", 3) }));
        r.push_back(Make("VertexColor", kWorld, "Vertex Color", "The vertex colour (alpha is 1).",
            {}, ChannelOutputs(), kNodeChannels));
        r.push_back(Make("TwoSidedSign", kWorld, "Two Sided Sign", "1 on a front face, -1 on a back face. Pixel stage only.",
            {}, { Out("Out", 1) }, kNodePixelOnly));
        r.push_back(Make("Time", kWorld, "Time", "Scene time in seconds; props.period > 0 wraps it.",
            {}, { Out("Out", 1) }));
        r.push_back(Make("Transform", kWorld, "Transform",
            "Transforms a direction. props.source / props.dest: Tangent, World or Local. "
            "Tangent space is pixel stage only.",
            { In("Input", 3, { 0.0f, 0.0f, 1.0f }) }, { Out("Out", 3) }));
        r.push_back(Make("PixelNormalWS", kWorld, "Pixel Normal WS",
            "The shading normal after the Normal output, world space - what the lighting uses (the vertex normal when "
            "Normal is unconnected). It cannot feed the Normal pin. Pixel stage only.",
            {}, { Out("Out", 3) }, kNodePixelOnly));
        r.push_back(Make("ScreenPosition", kWorld, "Screen Position",
            "ViewportUV: 0..1 across the view (the unjittered projection of the position); PixelPosition: the pixel "
            "coordinate in the render target (SV_Position.xy). Pixel stage only.",
            {}, { Out("ViewportUV", 2, "Viewport UV"), Out("PixelPosition", 2, "Pixel Position") }, kNodePixelOnly));
        r.push_back(Make("PixelDepth", kWorld, "Pixel Depth",
            "The pixel's view-space depth in metres (the clip-space w of its position). Pixel stage only.",
            {}, { Out("Out", 1) }, kNodePixelOnly));

        // ---- Utility ----
        r.push_back(Make("Fresnel", kUtil, "Fresnel",
            "Base + (1 - Base) * pow(1 - saturate(dot(Normal, V)), Exponent).",
            { In("ExponentIn", 1, { 5.0f }, "Exponent"), In("BaseReflectFractionIn", 1, { 0.04f }, "Base Reflect Fraction"),
              Implicit("Normal", 3, "p.geoNormalWS", "p.normalWS") },
            { Out("Out", 1) }));
        r.push_back(Make("SphereMask", kUtil, "Sphere Mask",
            "saturate((1 - |A - B| / Radius) / (1 - Hardness)); Hardness is 0..1.",
            { In("A", 0, { 0.0f }), In("B", 0, { 0.0f }), In("Radius", 1, { 1.0f }), In("Hardness", 1, { 0.8f }) },
            { Out("Out", 1) }));
        r.push_back(Make("Desaturation", kUtil, "Desaturation",
            "lerp(Input, luminance, Fraction). props.luminanceFactors [3] (0.3, 0.59, 0.11).",
            { In("Input", 3, { 0.0f, 0.0f, 0.0f }), In("Fraction", 1, { 1.0f }) }, { Out("Out", 3) }));
        r.push_back(Make("RotateAboutAxis", kUtil, "Rotate About Axis",
            "The OFFSET that rotates Position about the axis through PivotPoint by RotationAngle "
            "turns (1 = a full turn); add it to the position.",
            { In("NormalizedRotationAxis", 3, { 0.0f, 1.0f, 0.0f }, "Axis"), In("RotationAngle", 1, { 0.0f }, "Angle"),
              In("PivotPoint", 3, { 0.0f, 0.0f, 0.0f }, "Pivot"), Implicit("Position", 3, "p.worldPos", "p.worldPos") },
            { Out("Out", 3) }));
        r.push_back(Make("Reroute", kUtil, "Reroute", "Passes its input through.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        r.push_back(Make("NamedRerouteDeclaration", kUtil, "Named Reroute",
            "Names its input (props.name, props.color): a Named Reroute Usage of that name is this value anywhere in "
            "the graph, with no wire across it. Names are unique in a graph.",
            { In("Input", 0, { 0.0f }) }, { Out("Out", 0) }));
        {
            PinDesc declared = In("Input", 0, { 0.0f });
            declared.hidden = true;   // linked by the compiler to the declaration
            r.push_back(Make("NamedRerouteUsage", kUtil, "Named Reroute Usage",
                "The value of the Named Reroute declared as props.name.",
                { declared }, { Out("Out", 0) }));
        }

        // ---- Material Functions ----
        const char* kFunc = "Function";
        r.push_back(Make("FunctionInput", kFunc, "Function Input",
            "A Material Function's input: props.name is the call's pin, props.type its size (float .. float4), props.sort "
            "its order. Unconnected at the call, its Preview input is the value.",
            { In("Preview", 3, { 0.0f, 0.0f, 0.0f }) }, { Out("Out", 3) }, kNodeFunction));
        r.push_back(Make("FunctionOutput", kFunc, "Function Output",
            "A Material Function's output: props.name is the call's output pin, props.sort its order.",
            { In("Input", 0, { 0.0f }) }, {}, kNodeFunction));
        r.push_back(Make("MaterialFunctionCall", kFunc, "Material Function Call",
            "The .imatfunc at props.function, expanded here when compiled: its Function Inputs are this node's inputs, "
            "its Function Outputs its outputs (by props.sort, then name). A function calling itself is an error.",
            {}, {}));

        // ---- Custom ----
        r.push_back(Make("Custom", "Custom", "Custom",
            "HLSL. props.code is the body of a function returning props.outputType (float .. float4) - a body without "
            "'return' is one expression, returned; props.inputs declares its input pins ('A:float3 B:float'), each a "
            "parameter of that name; p (MaterialPixelParams, or MaterialVertexParams under World Position Offset) is in "
            "scope. No preprocessor lines: the engine headers are included already. DXC's errors land on this node.",
            { In("A", 3, { 0.0f, 0.0f, 0.0f }) }, { Out("Out", 3) }));

        // ---- Water (water_common.hlsli: real in the Water shading model, stubs elsewhere) ----
        const char* kWater = "Water";
        r.push_back(Make("SceneDepth", kWater, "Scene Depth",
            "Water shading model: Depth = metres from the camera to the opaque scene behind this pixel; WaterDepth = "
            "metres of water under the surface, vertically. 1e6 where nothing is behind - and in any other shading "
            "model (the stub). Pixel stage only.",
            {}, { Out("Depth", 1), Out("WaterDepth", 1, "Water Depth") }, kNodePixelOnly));
        r.push_back(Make("SceneColor", kWater, "Scene Color",
            "Water shading model: the lit opaque scene at UVs (unconnected: this pixel's screen UV) - refraction; "
            "Fallback where that is sky or off screen, and in any other shading model. Pixel stage only.",
            { Implicit("UVs", 2, "WaterScreenUV(p)", nullptr), In("Fallback", 3, { 0.0f, 0.0f, 0.0f }) },
            { Out("Out", 3) }, kNodePixelOnly));
        r.push_back(Make("WaterFresnel", kWater, "Water Fresnel",
            "Water shading model: Schlick's F0 + (1 - F0)(1 - N.V)^5 of Normal (unconnected: the vertex normal). "
            "Any other model: F0 (the stub). Pixel stage only.",
            { In("F0", 1, { 0.02f }), Implicit("Normal", 3, "p.geoNormalWS", nullptr) }, { Out("Out", 1) },
            kNodePixelOnly));

        // ---- Scene: Unreal's scene inputs; an error in a material whose pass lacks them ----
        const char* kScene = "Scene";
        r.push_back(Make("SceneTexture", kScene, "Scene Texture",
            "Unreal's post-process scene inputs. A surface material cannot read the scene it is drawn into: an error "
            "on the node - a Water material's Scene Depth / Scene Color are the counterparts.",
            { Implicit("UVs", 2, "p.uv0", "p.uv0") }, ChannelOutputs(), kNodeChannels));
        r.push_back(Make("SceneTexelSize", kScene, "Scene Texel Size",
            "1 / the render target's size. Only the Water pass knows it: an error in any other shading model. Pixel "
            "stage only.",
            {}, { Out("Out", 2) }, kNodePixelOnly));
        r.push_back(Make("SkyAtmosphereLightDirection", kScene, "Sky Atmosphere Light Direction",
            "The unit direction toward the sun. Only a translucent or Water material (the forward pass) has it: an "
            "opaque one is drawn into the GBuffer before any light - an error there (use a Vector Parameter).",
            {}, { Out("Out", 3) }, kNodePixelOnly));

        // ---- Engine ----
        r.push_back(Make("StandardSurface", kEngine, "Standard Surface",
            "The engine's standard surface - texture slots and schema parameters of the .imat - "
            "exactly as a material without a custom shader. Normal is WORLD space: connect it to an "
            "Output whose props.tangentSpaceNormal is false. Pixel stage only.",
            {},
            { Out("BaseColor", 3, "Base Color"), Out("Opacity", 1), Out("Normal", 3),
              Out("Roughness", 1), Out("Metallic", 1), Out("AmbientOcclusion", 1, "Ambient Occlusion"),
              Out("Specular", 1), Out("EmissiveColor", 3, "Emissive Color") },
            kNodePixelOnly));
        AttachProps(r);
        return r;
    }
} // namespace

const PinDesc* NodeDesc::Input(std::string_view name) const
{
    const int i = InputIndex(name);
    return i >= 0 ? &inputs[static_cast<std::size_t>(i)] : nullptr;
}

const PinDesc* NodeDesc::Output(std::string_view name) const
{
    const int i = OutputIndex(name);
    return i >= 0 ? &outputs[static_cast<std::size_t>(i)] : nullptr;
}

int NodeDesc::InputIndex(std::string_view name) const
{
    for (std::size_t i = 0; i < inputs.size(); ++i)
        if (name == inputs[i].name) return static_cast<int>(i);
    return -1;
}

int NodeDesc::OutputIndex(std::string_view name) const
{
    for (std::size_t i = 0; i < outputs.size(); ++i)
        if (name == outputs[i].name) return static_cast<int>(i);
    return -1;
}

const std::vector<NodeDesc>& NodeRegistry()
{
    static const std::vector<NodeDesc> s_nodes = Build();
    return s_nodes;
}

const NodeDesc* FindNodeDesc(std::string_view type)
{
    for (const NodeDesc& d : NodeRegistry())
        if (type == d.type) return &d;
    return nullptr;
}

int CustomTypeComponents(std::string_view type)
{
    std::string t(type);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "float" || t == "float1") return 1;
    if (t == "float2") return 2;
    if (t == "float3") return 3;
    if (t == "float4") return 4;
    return 0;
}

namespace
{
    // One built description per distinct Custom declaration: its pin names live
    // in `names` (a deque: growing it never moves the strings the pins point at).
    struct CustomDesc
    {
        NodeDesc                desc;
        std::deque<std::string> names;
    };

    bool IsHlslName(const std::string& s)
    {
        if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
        for (const char c : s)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
        return true;
    }

    // The function's own parameter p, and words an HLSL parameter cannot be named.
    bool IsReservedCustomName(const std::string& s)
    {
        static const char* const kWords[] = {
            "p", "return", "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
            "discard", "true", "false", "in", "out", "inout", "uniform", "static", "const", "struct", "void", "bool",
            "int", "uint", "half", "float", "float2", "float3", "float4", "double", "matrix", "vector", "register",
            "cbuffer", "sampler", "texture", "packoffset", "groupshared", "precise", "typedef", "namespace" };
        for (const char* w : kWords)
            if (s == w) return true;
        return false;
    }
}

bool ParseCustomInputs(std::string_view spec, std::vector<CustomInput>& out, std::string* error)
{
    out.clear();
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        if (ok && error) *error = msg;
        ok = false;
    };
    std::vector<std::string> tokens;
    std::string cur;
    for (const char c : spec)
    {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c)))
        {
            if (!cur.empty()) tokens.push_back(std::move(cur));
            cur.clear();
        }
        else cur += c;
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    for (const std::string& t : tokens)
    {
        const std::size_t colon = t.find(':');
        const std::string name  = t.substr(0, colon);
        const std::string type  = colon == std::string::npos ? std::string("float") : t.substr(colon + 1);
        const int n = CustomTypeComponents(type);
        if (!IsHlslName(name)) { fail("'" + name + "' is not an HLSL name"); continue; }
        if (IsReservedCustomName(name)) { fail("'" + name + "' is reserved (p is the stage's parameters; the rest are HLSL words)"); continue; }
        if (!n) { fail("'" + type + "' (of " + name + ") is not float, float2, float3 or float4"); continue; }
        bool dup = false;
        for (const CustomInput& i : out) dup = dup || i.name == name;
        if (dup) { fail("input '" + name + "' is declared twice"); continue; }
        if (static_cast<int>(out.size()) == kMaxCustomInputs) { fail("at most " + std::to_string(kMaxCustomInputs) + " inputs"); break; }
        out.push_back({ name, n });
    }
    return ok;
}

void FunctionPins(const Graph& fn, std::vector<FunctionPin>& inputs, std::vector<FunctionPin>& outputs)
{
    inputs.clear();
    outputs.clear();
    for (const Node& n : fn.nodes)
    {
        if (n.type == "FunctionInput")
        {
            int c = CustomTypeComponents(n.PropString("type", "float3"));
            inputs.push_back({ n.PropString("name", "In"), c ? c : 3, n.id, n.PropNumber("sort", 0.0) });
        }
        else if (n.type == "FunctionOutput")
            outputs.push_back({ n.PropString("name", "Result"), 0, n.id, n.PropNumber("sort", 0.0) });
    }
    auto order = [](const FunctionPin& a, const FunctionPin& b) {
        return a.sort != b.sort ? a.sort < b.sort : a.name != b.name ? a.name < b.name : a.node < b.node;
    };
    std::sort(inputs.begin(), inputs.end(), order);
    std::sort(outputs.begin(), outputs.end(), order);
}

namespace
{
    // One description per distinct pin signature (@p key), built by @p build and kept.
    template <class Build>
    const NodeDesc* BuiltDesc(const std::string& key, const char* baseType, Build&& build)
    {
        static std::mutex s_mutex;
        static std::unordered_map<std::string, std::unique_ptr<CustomDesc>> s_built;
        std::lock_guard<std::mutex> lock(s_mutex);
        std::unique_ptr<CustomDesc>& slot = s_built[key];
        if (!slot)
        {
            slot = std::make_unique<CustomDesc>();
            slot->desc = *FindNodeDesc(baseType);   // type, category, label, help, flags, props
            slot->desc.inputs.clear();
            slot->desc.outputs.clear();
            build(*slot);
        }
        return &slot->desc;
    }
}

const NodeDesc* DescribeNode(const Node& n)
{
    if (n.type == "Custom")
    {
        std::vector<CustomInput> inputs;
        ParseCustomInputs(n.PropString("inputs", kCustomDefaultInputs), inputs, nullptr);   // the compiler reports errors
        int outN = CustomTypeComponents(n.PropString("outputType", kCustomDefaultOutput));
        if (!outN) outN = CustomTypeComponents(kCustomDefaultOutput);
        std::string key = "Custom|";
        for (const CustomInput& i : inputs) key += i.name + ":" + std::to_string(i.components) + " ";
        key += "-> " + std::to_string(outN);
        return BuiltDesc(key, "Custom", [&](CustomDesc& c) {
            for (const CustomInput& i : inputs)
            {
                c.names.push_back(i.name);
                c.desc.inputs.push_back(In(c.names.back().c_str(), i.components, { 0.0f }));
            }
            c.desc.outputs = { Out("Out", outN) };
        });
    }
    if (n.type == "FunctionInput")
    {
        int comps = CustomTypeComponents(n.PropString("type", "float3"));
        if (!comps) comps = 3;
        return BuiltDesc("FunctionInput|" + std::to_string(comps), "FunctionInput", [&](CustomDesc& c) {
            c.desc.inputs  = { In("Preview", comps, { 0.0f }) };
            c.desc.outputs = { Out("Out", comps) };
        });
    }
    if (n.type == "MaterialFunctionCall")
    {
        // Its pins are its function's interface (the file, kept while unchanged).
        std::string error;
        const std::shared_ptr<const Graph> fn = LoadFunctionCached(n.PropString("function"), error);
        std::vector<FunctionPin> ins, outs;
        if (fn) FunctionPins(*fn, ins, outs);
        std::string key = "Call|";
        for (const FunctionPin& p : ins) key += p.name + ":" + std::to_string(p.components) + "\x1f";
        key += "|";
        for (const FunctionPin& p : outs) key += p.name + "\x1f";
        return BuiltDesc(key, "MaterialFunctionCall", [&](CustomDesc& c) {
            for (const FunctionPin& p : ins)
            {
                c.names.push_back(p.name);
                c.desc.inputs.push_back(Sink(c.names.back().c_str(), p.components, c.names.back().c_str()));
            }
            for (const FunctionPin& p : outs)
            {
                c.names.push_back(p.name);
                c.desc.outputs.push_back(Out(c.names.back().c_str(), 0));
            }
        });
    }
    return FindNodeDesc(n.type);
}
} // namespace MaterialGraph
