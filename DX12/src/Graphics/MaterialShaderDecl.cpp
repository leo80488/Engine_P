// MaterialShaderDecl.cpp - the material shader source parser; the contract is in the header.

#include "Graphics/MaterialShaderDecl.h"

#include "Graphics/PermutationKey.h"
#include "Graphics/ShaderDiskCache.h"
#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace MaterialShaderDecl
{
namespace
{
    using ShaderReflect::CBufferLayout;
    using ShaderReflect::CBufferVar;
    using ShaderReflect::Reflection;
    using ShaderReflect::ResourceBinding;
    using ShaderReflect::ResourceType;
    using ShaderReflect::TextureDim;
    using ShaderReflect::VarType;

    // ==== The //@ annotation grammar ====
    // ShaderLibrary stamps it onto every compiled shader's reflection (StampSourceAnnotations); the parser
    // below reads the same sections onto its own declarations.

    // Once per process and message: every library and permutation parses the
    // same source.
    void AnnoWarnOnce(const std::string& msg)
    {
        static std::mutex            s_mutex;
        static std::set<std::string> s_reported;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_reported.insert(msg).second) LOG_WARNING("%s", msg.c_str());
    }

    void ApplyAnnoSection(const std::string& sec, CBufferVar& var, const std::string& file)
    {
        // sec = "default 0.5 2.0 1.0 1.0"  /  "min 0 0 0 0"  /  'label "Off|Sharp"'.
        size_t i = 0;
        while (i < sec.size() && std::isspace((unsigned char)sec[i])) ++i;
        size_t k = i;
        while (k < sec.size() && !std::isspace((unsigned char)sec[k])) ++k;
        const std::string key = sec.substr(i, k - i);
        std::string rest = (k < sec.size()) ? sec.substr(k) : std::string();

        // `@engine <role>`: a parameter the engine reads by meaning. An unknown role is reported - it would
        // otherwise leave the material on the positional convention without a word.
        if (key == "engine")
        {
            std::istringstream rs(rest);
            std::string role;
            rs >> role;
            using namespace ShaderReflect;
            if (role == kEngineRoleFoliageTint || role == kEngineRoleFoliageWrap ||
                role == kEngineRoleFoliageStrength || role == kEngineRoleFaceShadow)
                var.engineRole = role;
            else
                AnnoWarnOnce("'" + file + "': parameter '" + var.name + "' has @engine '" + role +
                             "' - the parameter roles are foliageTint, foliageWrap, foliageStrength, faceShadow");
            return;
        }

        if (key == "label" || key == "group")
        {
            // The first quoted string wins; text outside the quotes (e.g. a trailing doc comment) is ignored.
            // Unquoted: the trimmed rest.
            std::string* dst = (key == "label") ? &var.label : &var.group;
            const size_t q1 = rest.find('"');
            if (q1 != std::string::npos)
            {
                const size_t q2 = rest.find('"', q1 + 1);
                if (q2 != std::string::npos) { *dst = rest.substr(q1 + 1, q2 - q1 - 1); return; }
            }
            const size_t a = rest.find_first_not_of(" \t");
            const size_t b = rest.find_last_not_of(" \t\r\n");
            if (a != std::string::npos) *dst = rest.substr(a, b - a + 1);
            return;
        }

        // numeric sections: parse up to 4 floats
        std::array<float, 4> vals = { 0, 0, 0, 0 };
        int n = 0;
        std::istringstream ts(rest);
        float f;
        while (n < 4 && (ts >> f)) vals[n++] = f;
        if (n == 0) return;
        // scalar shorthand: a single value fills [0] only (matches how scalar
        // params read .x); callers that want a vec must list all components.
        if (key == "default") { var.defaultValue = vals; var.hasDefault = true; }
        else if (key == "min") { var.minValue = vals; var.hasRange = true; }
        else if (key == "max") { var.maxValue = vals; var.hasRange = true; }
    }

    // A texture declaration's annotation (ShaderReflect::TextureDefault): `default
    // white|black|grey|normal`, `srgb`, `linear`. Anything else is reported - a misspelt default would
    // otherwise leave the slot white, the very trap the annotation exists to close.
    void ApplyTextureAnnoSection(const std::string& sec, ResourceBinding& b, const std::string& file)
    {
        std::istringstream ts(sec);
        std::string key, value;
        ts >> key >> value;
        // Once per process: every library and permutation parses the same source.
        auto warnOnce = [&](const std::string& what) {
            static std::mutex            s_mutex;
            static std::set<std::string> s_reported;
            const std::string msg = "'" + file + "': texture '" + b.name + "' " + what;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_reported.insert(msg).second) LOG_WARNING("%s", msg.c_str());
        };
        using TD = ShaderReflect::TextureDefault;
        if (key == "default")
        {
            if      (value == "white")                    b.texDefault = TD::White;
            else if (value == "black")                    b.texDefault = TD::Black;
            else if (value == "grey" || value == "gray")  b.texDefault = TD::Grey;
            else if (value == "normal")                   b.texDefault = TD::Normal;
            else warnOnce("has //@default '" + value + "' - expected white, black, grey or normal; "
                          "the slot stays white");
        }
        else if (key == "srgb")   b.colorSpace = ShaderReflect::TextureColorSpace::SRGB;
        else if (key == "linear") b.colorSpace = ShaderReflect::TextureColorSpace::Linear;
        else if (key == "engine")
        {
            // A texture the engine reads by meaning.
            if (value == ShaderReflect::kEngineRoleToonRamp) b.engineRole = value;
            else warnOnce("has @engine '" + value + "' - the texture roles are: toonRamp");
        }
        else if (!key.empty())    warnOnce("has an unknown annotation '@" + key + "' (default, srgb, linear, engine)");
    }

    // The identifier right before position @p end in @p line (spaces skipped), or "".
    std::string IdentifierBefore(const std::string& line, size_t end)
    {
        size_t e = end;
        while (e > 0 && std::isspace((unsigned char)line[e - 1])) --e;
        size_t b = e;
        while (b > 0 && (std::isalnum((unsigned char)line[b - 1]) || line[b - 1] == '_')) --b;
        return line.substr(b, e - b);
    }

    // The '@'-delimited sections of the text after a "//@" marker. The text
    // starts with a bare keyword, so a leading '@' is seeded.
    template <class F>
    void ForEachSection(const std::string& afterMarker, F&& f)
    {
        const std::string body = "@" + afterMarker;
        size_t pos = 0;
        while ((pos = body.find('@', pos)) != std::string::npos)
        {
            const size_t next = body.find('@', pos + 1);
            f(body.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1));
            if (next == std::string::npos) break;
            pos = next;
        }
    }

    // A line of its own `//@engine <value>`: whether it is the `explicit`
    // directive (and a warning when it is something else).
    bool EngineExplicitLine(const std::string& afterMarker, const std::string& file)
    {
        std::istringstream ds(afterMarker);
        std::string key, value;
        ds >> key >> value;
        if (key != "engine") return false;
        if (value == "explicit") return true;
        AnnoWarnOnce("'" + file + "': '//@engine " + value +
                     "' on a line of its own - the directive is '//@engine explicit'");
        return false;
    }

    // ==== The tokenizer ====
    struct Tok
    {
        enum Kind : uint8_t { Ident, Number, Punct, Str, End };
        Kind        kind = End;
        std::string text;
        uint32_t    line = 0;
    };

    struct Directive
    {
        std::string name;        // "include", "define", "if", ...
        std::string rest;        // the rest of the line, comments removed, trimmed
        uint32_t    line   = 0;
        size_t      before = 0;  // the index of the first code token after it
    };

    // A raw line's "//@" marker, found the way StampSourceAnnotations finds it
    // (the first "//@" on the line, wherever it is).
    struct LineNote
    {
        std::string body;                // the text after "//@"
        bool        alone      = false;  // the marker is the line's first non-blank
        bool        semiBefore = false;  // a ';' precedes it on the line
    };

    struct Lexed
    {
        std::vector<Tok>                       toks;   // ends with one End
        std::vector<Directive>                 dirs;
        std::unordered_map<uint32_t, LineNote> notes;  // by line
        uint32_t                               permBits = 0;
    };

    bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
    bool IsIdentChar(char c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    std::string Trim(const std::string& s)
    {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        const size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    void NamePermBit(const std::string& word, uint32_t& bits)
    {
        for (const PermutationDefine& d : kPermutationDefines)
            if (word == d.name) { bits |= d.bit; return; }
    }

    // Every whole word of @p text that names a permutation define (the rule
    // tools/gen_perm_relevance.py applies to comment-stripped text).
    void NamePermBits(const std::string& text, uint32_t& bits)
    {
        if (text.find("##") != std::string::npos) { bits = ~0u; return; }
        for (size_t i = 0; i < text.size();)
        {
            if (!IsIdentStart(text[i])) { ++i; continue; }
            size_t j = i;
            while (j < text.size() && IsIdentChar(text[j])) ++j;
            if (i == 0 || !IsIdentChar(text[i - 1])) NamePermBit(text.substr(i, j - i), bits);
            i = j;
        }
    }

    Lexed Lex(const std::string& src)
    {
        Lexed lx;

        // The raw lines' markers first: exactly the lines StampSourceAnnotations reads.
        {
            uint32_t ln = 1;
            size_t   b  = 0;
            while (b <= src.size())
            {
                size_t e = src.find('\n', b);
                if (e == std::string::npos) e = src.size();
                const std::string line = src.substr(b, e - b);
                const size_t at = line.find("//@");
                if (at != std::string::npos)
                {
                    LineNote n;
                    n.body       = line.substr(at + 3);
                    n.alone      = line.find_first_not_of(" \t") == at;
                    n.semiBefore = line.rfind(';', at) != std::string::npos;
                    lx.notes.emplace(ln, std::move(n));
                }
                if (e == src.size()) break;
                ++ln;
                b = e + 1;
            }
        }

        const size_t n = src.size();
        size_t   i = 0;
        uint32_t line = 1;
        bool     lineStart = true;
        auto skipBlockComment = [&](std::string* out) {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/'))
            {
                if (src[i] == '\n') ++line;
                ++i;
            }
            i = std::min(n, i + 2);
            if (out) out->push_back(' ');
        };
        while (i < n)
        {
            const char c = src[i];
            if (c == '\n') { ++line; lineStart = true; ++i; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++i; continue; }
            if (c == '\\' && i + 1 < n && src[i + 1] == '\n') { i += 2; ++line; continue; }
            if (c == '\\' && i + 2 < n && src[i + 1] == '\r' && src[i + 2] == '\n') { i += 3; ++line; continue; }
            if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') ++i; continue; }
            if (c == '/' && i + 1 < n && src[i + 1] == '*') { skipBlockComment(nullptr); continue; }

            if (c == '#' && lineStart)
            {
                // A directive: to the end of the line, backslash continuations joined.
                Directive d;
                d.line   = line;
                d.before = lx.toks.size();
                std::string text;
                ++i;
                while (i < n)
                {
                    if (src[i] == '\\' && i + 1 < n && src[i + 1] == '\n') { i += 2; ++line; continue; }
                    if (src[i] == '\\' && i + 2 < n && src[i + 1] == '\r' && src[i + 2] == '\n') { i += 3; ++line; continue; }
                    if (src[i] == '\n') break;
                    if (src[i] == '/' && i + 1 < n && src[i + 1] == '*') { skipBlockComment(&text); continue; }
                    if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') ++i; break; }
                    text.push_back(src[i]);
                    ++i;
                }
                NamePermBits(text, lx.permBits);
                const size_t a = text.find_first_not_of(" \t\r");
                size_t e = (a == std::string::npos) ? text.size() : a;
                while (e < text.size() && IsIdentChar(text[e])) ++e;
                d.name = (a == std::string::npos) ? std::string() : text.substr(a, e - a);
                d.rest = Trim(text.substr(e));
                lx.dirs.push_back(std::move(d));
                continue;   // the '\n' that ends it starts the next line
            }

            lineStart = false;
            Tok t;
            t.line = line;
            if (c == '"' || c == '\'')
            {
                size_t j = i + 1;
                while (j < n && src[j] != c && src[j] != '\n')
                {
                    if (src[j] == '\\' && j + 1 < n) ++j;
                    ++j;
                }
                const size_t end = std::min(n, j + 1);
                t.kind = Tok::Str;
                t.text = src.substr(i, end - i);
                NamePermBits(t.text, lx.permBits);
                i = end;
            }
            else if (IsIdentStart(c))
            {
                size_t j = i;
                while (j < n && IsIdentChar(src[j])) ++j;
                t.kind = Tok::Ident;
                t.text = src.substr(i, j - i);
                NamePermBit(t.text, lx.permBits);
                i = j;
            }
            else if (std::isdigit(static_cast<unsigned char>(c)) ||
                     (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1]))))
            {
                const bool hex = c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X');
                size_t j = i;
                while (j < n && (IsIdentChar(src[j]) || src[j] == '.' ||
                                 (!hex && (src[j] == '+' || src[j] == '-') && j > i &&
                                  (src[j - 1] == 'e' || src[j - 1] == 'E'))))
                    ++j;
                t.kind = Tok::Number;
                t.text = src.substr(i, j - i);
                i = j;
            }
            else
            {
                t.kind = Tok::Punct;
                t.text = std::string(1, c);
                ++i;
            }
            lx.toks.push_back(std::move(t));
        }
        Tok end;
        end.kind = Tok::End;
        end.line = line;
        lx.toks.push_back(std::move(end));
        return lx;
    }

    // ==== Types ====
    struct TypeInfo
    {
        bool         known    = false;   // numeric: the packer can lay it out
        bool         resource = false;
        // numeric
        VarType      vt     = VarType::Unknown;
        uint32_t     rows   = 1;         // a scalar / vector is 1 x cols
        uint32_t     cols   = 1;
        bool         matrix = false;
        // resource
        ResourceType rtype    = ResourceType::Other;
        TextureDim   dim      = TextureDim::Unknown;
        char         regClass = 0;       // 't' 's' 'b' 'u'
        bool         templated = false;  // ConstantBuffer<T>: its layout is T's
        std::string  why;                // numeric but not laid out: why
    };

    // float / int / uint / dword / bool, a vector (float3) or a matrix (float3x4,
    // `matrix` = float4x4); the 16- / 64-bit and min-precision types are named
    // and left to the DXIL (their cbuffer size depends on -enable-16bit-types).
    bool NumericType(const std::string& s, TypeInfo& t)
    {
        static const char* const kOdd[] = {
            "half", "double", "min16float", "min10float", "min16int", "min12int", "min16uint",
            "int16_t", "uint16_t", "int64_t", "uint64_t", "float16_t", "float64_t", "float32_t",
            "int32_t", "uint32_t", "vector",
        };
        for (const char* o : kOdd)
        {
            const size_t len = std::strlen(o);
            if (s.compare(0, len, o) == 0 &&
                (s.size() == len || std::isdigit(static_cast<unsigned char>(s[len]))))
            {
                t.known = false;
                t.why   = "type '" + s + "' (its cbuffer size is left to the DXIL)";
                return true;
            }
        }
        if (s == "matrix") { t.known = true; t.vt = VarType::Float4x4; t.rows = 4; t.cols = 4; t.matrix = true; return true; }
        static const struct { const char* name; char kind; } kBase[] = {
            { "float", 'f' }, { "uint", 'u' }, { "dword", 'u' }, { "int", 'i' }, { "bool", 'b' },
        };
        for (const auto& b : kBase)
        {
            const size_t len = std::strlen(b.name);
            if (s.compare(0, len, b.name) != 0) continue;
            const std::string dims = s.substr(len);
            uint32_t rows = 1, cols = 1;
            bool matrix = false;
            if (dims.empty()) {}
            else if (dims.size() == 1 && dims[0] >= '1' && dims[0] <= '4') cols = static_cast<uint32_t>(dims[0] - '0');
            else if (dims.size() == 3 && dims[0] >= '1' && dims[0] <= '4' && dims[1] == 'x' &&
                     dims[2] >= '1' && dims[2] <= '4')
            {
                rows = static_cast<uint32_t>(dims[0] - '0');
                cols = static_cast<uint32_t>(dims[2] - '0');
                matrix = true;
            }
            else continue;   // "intersection", "floatValue": not a type
            t.known  = true;
            t.rows   = rows;
            t.cols   = cols;
            t.matrix = matrix;
            // As ShaderReflect's MapVarType: a matrix is Float4x4 or nothing; a bool of any width is Bool.
            if (matrix) t.vt = (b.kind == 'f' && rows == 4 && cols == 4) ? VarType::Float4x4 : VarType::Unknown;
            else if (b.kind == 'b') t.vt = VarType::Bool;
            else
            {
                static const VarType kF[] = { VarType::Float, VarType::Float2, VarType::Float3, VarType::Float4 };
                static const VarType kI[] = { VarType::Int,   VarType::Int2,   VarType::Int3,   VarType::Int4 };
                static const VarType kU[] = { VarType::UInt,  VarType::UInt2,  VarType::UInt3,  VarType::UInt4 };
                t.vt = (b.kind == 'f' ? kF : b.kind == 'i' ? kI : kU)[cols - 1];
            }
            return true;
        }
        return false;
    }

    // ShaderReflect's MapResourceType / MapDim, read from the type's name.
    bool ResourceTypeOf(const std::string& s, TypeInfo& t)
    {
        static const struct { const char* name; ResourceType type; TextureDim dim; char reg; } kTypes[] = {
            { "Texture1D",               ResourceType::Texture,            TextureDim::Tex1D,        't' },
            { "Texture1DArray",          ResourceType::Texture,            TextureDim::Tex1DArray,   't' },
            { "Texture2D",               ResourceType::Texture,            TextureDim::Tex2D,        't' },
            { "Texture2DArray",          ResourceType::Texture,            TextureDim::Tex2DArray,   't' },
            { "Texture2DMS",             ResourceType::Texture,            TextureDim::Tex2DMS,      't' },
            { "Texture2DMSArray",        ResourceType::Texture,            TextureDim::Tex2DMSArray, 't' },
            { "Texture3D",               ResourceType::Texture,            TextureDim::Tex3D,        't' },
            { "TextureCube",             ResourceType::Texture,            TextureDim::TexCube,      't' },
            { "TextureCubeArray",        ResourceType::Texture,            TextureDim::TexCubeArray, 't' },
            { "Buffer",                  ResourceType::Texture,            TextureDim::Buffer,       't' },
            { "RWTexture1D",             ResourceType::RWTexture,          TextureDim::Tex1D,        'u' },
            { "RWTexture1DArray",        ResourceType::RWTexture,          TextureDim::Tex1DArray,   'u' },
            { "RWTexture2D",             ResourceType::RWTexture,          TextureDim::Tex2D,        'u' },
            { "RWTexture2DArray",        ResourceType::RWTexture,          TextureDim::Tex2DArray,   'u' },
            { "RWTexture3D",             ResourceType::RWTexture,          TextureDim::Tex3D,        'u' },
            { "RWBuffer",                ResourceType::RWTexture,          TextureDim::Buffer,       'u' },
            { "StructuredBuffer",        ResourceType::StructuredBuffer,   TextureDim::Buffer,       't' },
            { "RWStructuredBuffer",      ResourceType::RWStructuredBuffer, TextureDim::Buffer,       'u' },
            { "AppendStructuredBuffer",  ResourceType::Other,              TextureDim::Buffer,       'u' },
            { "ConsumeStructuredBuffer", ResourceType::Other,              TextureDim::Buffer,       'u' },
            { "ByteAddressBuffer",       ResourceType::ByteAddressBuffer,  TextureDim::Buffer,       't' },
            { "RWByteAddressBuffer",     ResourceType::ByteAddressBuffer,  TextureDim::Buffer,       'u' },
            { "SamplerState",            ResourceType::Sampler,            TextureDim::Unknown,      's' },
            { "SamplerComparisonState",  ResourceType::Sampler,            TextureDim::Unknown,      's' },
            { "sampler",                 ResourceType::Sampler,            TextureDim::Unknown,      's' },
            { "RaytracingAccelerationStructure", ResourceType::Other,      TextureDim::Unknown,      't' },
            { "ConstantBuffer",          ResourceType::CBuffer,            TextureDim::Unknown,      'b' },
        };
        for (const auto& k : kTypes)
            if (s == k.name)
            {
                t.resource  = true;
                t.rtype     = k.type;
                t.dim       = k.dim;
                t.regClass  = k.reg;
                t.templated = (k.type == ResourceType::CBuffer);
                return true;
            }
        if (s.rfind("RasterizerOrdered", 0) == 0 || s.rfind("FeedbackTexture", 0) == 0)
        {
            t.resource = true;
            t.rtype    = ResourceType::Other;
            t.regClass = (s[0] == 'R') ? 'u' : 't';
            return true;
        }
        return false;
    }

    bool IsModifier(const std::string& s, bool& rowMajor, bool& colMajor, bool& isStatic, bool& isShared)
    {
        static const char* const kMods[] = {
            "const", "uniform", "extern", "precise", "volatile", "inline", "export", "nointerpolation",
            "globallycoherent", "linear", "centroid", "noperspective", "sample", "snorm", "unorm", "shared",
        };
        if (s == "row_major")    { rowMajor = true; return true; }
        if (s == "column_major") { colMajor = true; return true; }
        if (s == "static")       { isStatic = true; return true; }
        if (s == "groupshared")  { isShared = true; return true; }
        for (const char* m : kMods) if (s == m) return true;
        return false;
    }

    uint32_t RoundUp16(uint32_t v) { return (v + 15u) & ~15u; }

    // ==== The parser ====
    bool IsUnder(const std::string& path, const std::string& dir)
    {
        std::string p = Sys::PathToUtf8(Sys::U8Path(path).lexically_normal());
        std::string d = Sys::PathToUtf8(Sys::U8Path(dir).lexically_normal());
        auto fold = [](std::string& s) {
            for (char& c : s) c = (c == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        fold(p);
        fold(d);
        if (!d.empty() && d.back() != '/') d.push_back('/');
        return p.compare(0, d.size(), d) == 0;
    }

    std::string Normal(const std::string& path)
    {
        std::string s = Sys::PathToUtf8(Sys::U8Path(path).lexically_normal());
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    }

    // -matdecl_break (harness): every parsed cbuffer member sits 4 bytes later
    // than the packing rules put it - the reverse arm that proves a clean
    // CrossCheck is the check passing, not the check not running.
    bool DeclBroken()
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const bool s_broken = Sys::CommandLine::Get().Has("-matdecl_break");
        return s_broken;
#else
        return false;
#endif
    }

    struct Member
    {
        CBufferVar var;
        TypeInfo   type;
        bool       rowMajor = false;
        uint32_t   arrayLen = 0;          // 0 = not an array
        uint32_t   line     = 0;
        bool       annotatable = false;   // the identifier right before its statement's ';'
        uint32_t   semiLine = 0;
    };

    struct Ctx
    {
        Decl&                 decl;
        const std::string&    shaderDir;
        const ReadFn&         read;
        std::set<std::string> seen;       // own files parsed (lower-case, normalised)
        std::set<std::string> macros;     // names an own file #defines
        std::set<std::string> structs;    // struct / typedef names an own file declares
    };

    class FileParser
    {
    public:
        FileParser(Ctx& ctx, std::string path, const Lexed& lx)
            : m_ctx(ctx), m_path(std::move(path)), m_lx(lx) {}

        void Run()
        {
            // An annotation alone on its line, or after a ';': the engineRolesExplicit flag, read as
            // StampSourceAnnotations reads it.
            const bool root = m_path == m_ctx.decl.root;
            for (const auto& [ln, note] : m_lx.notes)
            {
                const bool explicitRoles =
                    note.alone ? EngineExplicitLine(note.body, m_path)
                               : note.semiBefore && ("//@" + note.body).find("@engine") != std::string::npos;
                if (!explicitRoles) continue;
                m_ctx.decl.iface.engineRolesExplicit = true;
                if (root) m_ctx.decl.rootEngineRolesExplicit = true;
            }
            // The frame's time, read through a hook's parameters (`p.time`) or the engine constant
            // (gTimeParams): the declaration has it before any variant compiles, which the material
            // record written at a slot's first sight needs. CrossCheck holds it to the DXIL's flag.
            for (size_t i = 0; i < m_lx.toks.size() && !m_ctx.decl.iface.readsFrameTime; ++i)
            {
                const Tok& t = m_lx.toks[i];
                if (t.kind != Tok::Ident) continue;
                if (t.text == "gTimeParams" || (t.text == "time" && i > 0 && IsP(m_lx.toks[i - 1], '.')))
                    m_ctx.decl.iface.readsFrameTime = true;
            }

            while (true)
            {
                Sync();
                const Tok& t = Peek();
                if (t.kind == Tok::End) break;
                if (IsP(t, ';')) { Next(); continue; }
                if (IsP(t, '[')) { SkipBalanced('[', ']'); continue; }   // [numthreads(...)], [earlydepthstencil]
                if (IsP(t, '{')) { SkipBalanced('{', '}'); continue; }
                if (t.kind != Tok::Ident) { Next(); continue; }
                if (t.text == "cbuffer" || t.text == "tbuffer") { CBuffer(); continue; }
                if (t.text == "struct" || t.text == "class" || t.text == "interface") { Struct(); continue; }
                if (t.text == "typedef") { Typedef(); continue; }
                if (t.text == "namespace")
                {
                    Next();
                    if (Peek().kind == Tok::Ident) Next();
                    if (IsP(Peek(), '{')) SkipBalanced('{', '}');
                    continue;
                }
                if (t.text == "template")
                {
                    Next();
                    if (IsP(Peek(), '<')) SkipBalanced('<', '>');
                    continue;
                }
                Declaration();
            }
            Sync();
        }

        static void ParseFile(Ctx& ctx, const std::string& path, const std::string& text)
        {
            std::string key = Normal(path);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            if (!ctx.seen.insert(key).second) return;   // #pragma once / include guards
            ctx.decl.files.push_back(Normal(path));
            const Lexed lx = Lex(text);
            if (lx.permBits == ~0u) ctx.decl.permBits = ~0u;
            else                    ctx.decl.permBits |= lx.permBits;
            FileParser p(ctx, Normal(path), lx);
            p.Run();
        }

    private:
        const Tok& Peek(size_t k = 0) const
        {
            const size_t i = std::min(m_ti + k, m_lx.toks.size() - 1);
            return m_lx.toks[i];
        }
        void Next() { if (m_ti + 1 < m_lx.toks.size()) ++m_ti; }
        static bool IsP(const Tok& t, char c) { return t.kind == Tok::Punct && t.text.size() == 1 && t.text[0] == c; }

        uint32_t RealDepth() const
        {
            return static_cast<uint32_t>(std::count(m_conds.begin(), m_conds.end(), false));
        }

        std::string Where(uint32_t line) const { return m_path + ":" + std::to_string(line); }
        void Inexact(uint32_t line, const std::string& why) { m_ctx.decl.inexact.push_back(Where(line) + ": " + why); }
        void Note(uint32_t line, const std::string& why)    { m_ctx.decl.notes.push_back(Where(line) + ": " + why); }

        static std::string FirstWord(const std::string& s)
        {
            size_t e = 0;
            while (e < s.size() && IsIdentChar(s[e])) ++e;
            return s.substr(0, e);
        }

        // Every directive that comes before the current token.
        void Sync()
        {
            while (m_di < m_lx.dirs.size() && m_lx.dirs[m_di].before <= m_ti)
            {
                OnDirective(m_lx.dirs[m_di]);
                ++m_di;
            }
        }

        void OnDirective(const Directive& d)
        {
            if (d.name == "ifdef" || d.name == "ifndef" || d.name == "if" || d.name == "elif") PermDefineTest(d);
            if (d.name == "ifndef")
            {
                // An include guard: #ifndef X immediately followed by #define X.
                bool guard = false;
                if (m_di + 1 < m_lx.dirs.size())
                {
                    const auto& nx = m_lx.dirs[m_di + 1];
                    guard = nx.name == "define" && nx.before == d.before && FirstWord(nx.rest) == FirstWord(d.rest);
                }
                m_conds.push_back(guard);
            }
            else if (d.name == "if" || d.name == "ifdef") m_conds.push_back(false);
            else if (d.name == "elif" || d.name == "else") { if (!m_conds.empty()) m_conds.back() = false; }
            else if (d.name == "endif") { if (!m_conds.empty()) m_conds.pop_back(); }
            else if (d.name == "define" || d.name == "undef")
            {
                const std::string name = FirstWord(d.rest);
                const bool on = d.name == "define";
                if (on) m_ctx.macros.insert(name);
                Decl& dc = m_ctx.decl;
                bool* cap = name == "MATERIAL_HAS_SURFACE"       ? &dc.hasSurface
                          : name == "MATERIAL_HAS_VERTEX_OFFSET" ? &dc.hasVertexOffset
                          : name == "MATERIAL_HAS_VERTEX_MODIFY" ? &dc.hasVertexModify : nullptr;
                if (cap) *cap = on;
                if (name == "MATERIAL_DOMAIN_WATER") dc.domain = on ? Domain::Water : Domain::Surface;
                if ((cap || name == "MATERIAL_DOMAIN_WATER") && RealDepth() > 0)
                    Note(d.line, "'" + name + "' is " + (on ? "defined" : "undefined") +
                                 " inside #if - read as if the branch were taken");
            }
            else if (d.name == "include") Include(d);
        }

        // A permutation define is always defined (ShaderLibrary passes every one as
        // 0 or 1): #ifdef / #ifndef of one, or `defined(X)` with no other mention of
        // X in the condition, is a constant - almost certainly not what was meant.
        void PermDefineTest(const Directive& d)
        {
            auto isPerm = [](const std::string& w) {
                for (const PermutationDefine& p : kPermutationDefines) if (w == p.name) return true;
                return false;
            };
            if (d.name == "ifdef" || d.name == "ifndef")
            {
                const std::string w = FirstWord(d.rest);
                if (isPerm(w))
                    m_ctx.decl.warnings.push_back(Where(d.line) + ": #" + d.name + " " + w + " is always " +
                                                  (d.name == "ifdef" ? "true" : "false") + " - every permutation define "
                                                  "is passed as 0 or 1; test it with #if " + w);
                return;
            }
            // Every word of the condition, and the ones that are defined()'s operand.
            std::vector<std::string> words, operands;
            const std::string& s = d.rest;
            for (size_t i = 0; i < s.size();)
            {
                if (!IsIdentStart(s[i])) { ++i; continue; }
                size_t j = i;
                while (j < s.size() && IsIdentChar(s[j])) ++j;
                words.push_back(s.substr(i, j - i));
                i = j;
            }
            for (size_t k = 0; k + 1 < words.size(); ++k)
                if (words[k] == "defined") operands.push_back(words[k + 1]);
            for (const std::string& w : operands)
                if (isPerm(w) && std::count(words.begin(), words.end(), w) == std::count(operands.begin(), operands.end(), w))
                    m_ctx.decl.warnings.push_back(Where(d.line) + ": defined(" + w + ") is always true - every "
                                                  "permutation define is passed as 0 or 1; test it with defined(" + w +
                                                  ") && " + w);
        }

        void EngineInclude(const std::string& name)
        {
            // The rule tools/gen_perm_relevance.py applies: an angle include of a
            // name that is no HLSL file (a C++ header in a __cplusplus branch) is
            // not a shader include.
            const std::string lower = [&] { std::string l = name; for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return l; }();
            const bool hlsl = (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".hlsl") == 0) ||
                              (lower.size() > 6 && lower.compare(lower.size() - 6, 6, ".hlsli") == 0);
            if (!hlsl && name.find('/') == std::string::npos) return;
            const std::string path = Normal(m_ctx.shaderDir + name);
            // Resolved as the generator's closure() resolves it: a file under the
            // shader root. One that leaves the root ("../TextFX.hlsli" from
            // asset/Shaders/text_fx/, which DxcCompiler's include handler still
            // finds by its base name) or is not there is unresolved.
            std::string text;
            if (!IsUnder(path, m_ctx.shaderDir) || !m_ctx.read(path, text))
            {
                auto& un = m_ctx.decl.unresolvedIncludes;
                if (std::find(un.begin(), un.end(), name) == un.end()) un.push_back(name);
                return;
            }
            auto& inc = m_ctx.decl.engineIncludes;
            if (std::find(inc.begin(), inc.end(), path) == inc.end()) inc.push_back(path);
        }

        void Include(const Directive& d)
        {
            const std::string& r = d.rest;
            if (r.size() < 2 || (r[0] != '"' && r[0] != '<')) return;
            const bool angle = r[0] == '<';
            const size_t end = r.find(angle ? '>' : '"', 1);
            if (end == std::string::npos) return;
            const std::string name = r.substr(1, end - 1);
            if (angle) { EngineInclude(name); return; }   // under the shader root: the engine's
            // DXC's search order (DxcCompiler's include handler): the including file's folder, then the root
            // source's (the first -I), then the shader root. A hit outside the shader root is the material's own;
            // the shader root's is the engine's.
            for (const std::string& dir : { m_path, m_ctx.decl.root })
            {
                const std::string cand = Normal(Sys::PathToUtf8(Sys::U8Path(dir).parent_path() / Sys::U8Path(name)));
                if (IsUnder(cand, m_ctx.shaderDir))
                {
                    // A relative path into the shader root: that engine source.
                    std::string rel = cand.substr(Normal(m_ctx.shaderDir).size());
                    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
                    EngineInclude(rel);
                    return;
                }
                std::string text;
                if (!m_ctx.read(cand, text)) continue;
                if (RealDepth() > 0) Inexact(d.line, "#include \"" + name + "\" inside #if");
                if (m_inCBuffer)     Inexact(d.line, "#include \"" + name + "\" inside a cbuffer");
                ParseFile(m_ctx, cand, text);
                return;
            }
            EngineInclude(name);   // not next to an own file: the shader root's
        }

        void SkipBalanced(char open, char close)
        {
            int depth = 0;
            while (Peek().kind != Tok::End)
            {
                Sync();
                const Tok& t = Peek();
                if (IsP(t, open)) ++depth;
                else if (IsP(t, close)) { --depth; if (depth <= 0) { Next(); return; } }
                Next();
            }
        }

        // To the ';' that ends this statement (nesting respected), consumed.
        void SkipStatement()
        {
            int paren = 0, brace = 0, bracket = 0;
            while (Peek().kind != Tok::End)
            {
                Sync();
                const Tok& t = Peek();
                if (IsP(t, '(')) ++paren;
                else if (IsP(t, ')')) --paren;
                else if (IsP(t, '{')) ++brace;
                else if (IsP(t, '}')) { --brace; if (brace < 0) return; }   // the enclosing block's: not ours
                else if (IsP(t, '[')) ++bracket;
                else if (IsP(t, ']')) --bracket;
                else if (IsP(t, ';') && paren <= 0 && brace <= 0 && bracket <= 0) { Next(); return; }
                Next();
            }
        }

        // `register ( t3 , space3 )` after the ':'. False (and nothing read past
        // the parentheses) when an argument is not a literal register / space.
        bool Register(char& cls, uint32_t& index, uint32_t& space, uint32_t line)
        {
            if (!(Peek().kind == Tok::Ident && Peek().text == "register" && IsP(Peek(1), '('))) return false;
            Next();
            const size_t open = m_ti;
            SkipBalanced('(', ')');
            std::vector<std::string> args;
            std::string cur;
            for (size_t k = open + 1; k + 1 < m_ti; ++k)
            {
                const Tok& t = m_lx.toks[k];
                if (IsP(t, ',')) { args.push_back(cur); cur.clear(); }
                else cur += t.text;
            }
            args.push_back(cur);
            auto literal = [](const std::string& a, char& c, uint32_t& v) {
                if (a.size() < 2 || !std::isalpha(static_cast<unsigned char>(a[0]))) return false;
                for (size_t k = 1; k < a.size(); ++k) if (!std::isdigit(static_cast<unsigned char>(a[k]))) return false;
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(a[0])));
                v = static_cast<uint32_t>(std::stoul(a.substr(1)));
                return true;
            };
            if (args.empty() || args.size() > 2 || !literal(args[0], cls, index))
            {
                Inexact(line, "register(" + (args.empty() ? std::string() : args[0]) + ") is not a literal register");
                return false;
            }
            space = 0;
            if (args.size() == 2)
            {
                const std::string& s = args[1];
                if (s.rfind("space", 0) != 0 || s.size() == 5 ||
                    !std::all_of(s.begin() + 5, s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
                {
                    Inexact(line, "register(..., " + s + ") is not a literal space");
                    return false;
                }
                space = static_cast<uint32_t>(std::stoul(s.substr(5)));
            }
            return true;
        }

        // `[N]` suffixes after a declarator's name. Returns false when one is not
        // a literal; @p unbounded is set by `[]`.
        bool ArraySuffix(std::vector<uint32_t>& dims, bool& unbounded, uint32_t line)
        {
            unbounded = false;
            bool ok = true;
            while (IsP(Peek(), '['))
            {
                Next();
                if (IsP(Peek(), ']')) { unbounded = true; Next(); continue; }
                const Tok& n = Peek();
                if (n.kind == Tok::Number && IsP(Peek(1), ']') &&
                    std::all_of(n.text.begin(), n.text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
                {
                    dims.push_back(static_cast<uint32_t>(std::stoul(n.text)));
                    Next();
                    Next();
                    continue;
                }
                Inexact(line, "an array size that is not a literal ('" + n.text + "')");
                ok = false;
                int depth = 1;
                while (Peek().kind != Tok::End && depth > 0)
                {
                    if (IsP(Peek(), '[')) ++depth;
                    else if (IsP(Peek(), ']')) --depth;
                    Next();
                }
            }
            return ok;
        }

        void Struct()
        {
            const uint32_t line = Peek().line;
            Next();
            if (Peek().kind == Tok::Ident) { m_ctx.structs.insert(Peek().text); Next(); }
            if (IsP(Peek(), ':')) { while (Peek().kind != Tok::End && !IsP(Peek(), '{') && !IsP(Peek(), ';')) Next(); }
            if (IsP(Peek(), '{')) SkipBalanced('{', '}');
            if (Peek().kind == Tok::Ident)
            {
                // `struct S { ... } s;`: a global of the struct's type.
                Inexact(line, "'" + Peek().text + "': a global outside any cbuffer (it lands in $Globals)");
                SkipStatement();
                return;
            }
            if (IsP(Peek(), ';')) Next();
        }

        void Typedef()
        {
            Next();
            std::string last;
            while (Peek().kind != Tok::End && !IsP(Peek(), ';'))
            {
                if (Peek().kind == Tok::Ident) last = Peek().text;
                if (IsP(Peek(), '<')) { SkipBalanced('<', '>'); continue; }
                Next();
            }
            if (!last.empty()) m_ctx.structs.insert(last);
            if (IsP(Peek(), ';')) Next();
        }

        void CBuffer()
        {
            const uint32_t line = Peek().line;
            const uint32_t depth = RealDepth();
            Next();
            if (Peek().kind != Tok::Ident) { Inexact(line, "a cbuffer without a name"); SkipStatement(); return; }
            CBufferLayout cb;
            cb.name = Peek().text;
            Next();
            ResourceBinding b;
            b.name = cb.name;
            b.type = ResourceType::CBuffer;
            bool registered = false;
            if (IsP(Peek(), ':'))
            {
                Next();
                char cls = 0;
                uint32_t idx = 0, space = 0;
                if (Register(cls, idx, space, line))
                {
                    if (cls != 'b') Inexact(line, "cbuffer '" + cb.name + "' at register class '" + std::string(1, cls) + "'");
                    registered = true;
                    b.bindPoint = cb.bindPoint = idx;
                    b.space     = cb.space     = space;
                }
            }
            if (!registered) Inexact(line, "cbuffer '" + cb.name + "' has no literal register: DXC assigns one");
            if (depth > 0)   Inexact(line, "cbuffer '" + cb.name + "' inside #if");
            if (!IsP(Peek(), '{')) { Inexact(line, "cbuffer '" + cb.name + "' has no body"); SkipStatement(); return; }
            Next();

            std::vector<Member> members;
            m_inCBuffer = true;
            while (true)
            {
                Sync();
                const Tok& t = Peek();
                if (t.kind == Tok::End) { Inexact(line, "cbuffer '" + cb.name + "' never closes"); break; }
                if (IsP(t, '}')) { Next(); break; }
                if (IsP(t, ';')) { Next(); continue; }
                if (RealDepth() > depth) Inexact(t.line, "a member of cbuffer '" + cb.name + "' inside #if");
                MemberStatement(cb.name, members);
            }
            m_inCBuffer = false;
            if (IsP(Peek(), ';')) Next();

            // ---- The HLSL cbuffer packing rules ----
            // A member never straddles a 16-byte register; an array element, a matrix and a struct start on one;
            // the last element of an array or column of a matrix is not padded, so a scalar can follow in its
            // register. column_major is the default (no -Zpr in DxcCompiler).
            uint32_t offset = 0;
            for (Member& m : members)
            {
                uint32_t elem = 0;
                bool aligned = false;
                uint32_t vectors = 1;
                if (m.type.matrix)
                {
                    const bool rowMajor = m.rowMajor;
                    vectors = rowMajor ? m.type.rows : m.type.cols;
                    const uint32_t comps = rowMajor ? m.type.cols : m.type.rows;
                    elem = (vectors - 1) * 16u + comps * 4u;
                    aligned = true;
                }
                else elem = m.type.cols * 4u;
                uint32_t size = elem;
                if (m.arrayLen > 0)
                {
                    const uint32_t stride = m.type.matrix ? vectors * 16u : RoundUp16(elem);
                    size = (m.arrayLen - 1) * stride + elem;
                    aligned = true;
                }
                uint32_t start = offset;
                if (aligned || (start % 16u) + size > 16u) start = RoundUp16(offset);
                m.var.offset   = start;
                m.var.size     = size;
                m.var.elements = m.arrayLen;
                m.var.type     = m.type.vt;
                offset = start + size;
            }
            cb.sizeBytes = RoundUp16(offset);
            if (DeclBroken())
                for (Member& m : members) m.var.offset += 4u;

            // ---- Annotations: the //@ of the line whose ';' ends the member ----
            for (Member& m : members)
            {
                if (!m.annotatable) continue;
                const auto it = m_lx.notes.find(m.semiLine);
                if (it == m_lx.notes.end() || it->second.alone || !it->second.semiBefore) continue;
                ForEachSection(it->second.body, [&](const std::string& sec) { ApplyAnnoSection(sec, m.var, m_path); });
            }
            for (Member& m : members)
            {
                m_ctx.decl.sites.push_back({ cb.name + "." + m.var.name, m_path, m.line });
                cb.vars.push_back(std::move(m.var));
            }
            m_ctx.decl.sites.push_back({ cb.name, m_path, line });
            m_ctx.decl.iface.bindings.push_back(std::move(b));
            m_ctx.decl.iface.cbuffers.push_back(std::move(cb));
        }

        // One member statement inside a cbuffer: `[modifiers] type a [N] [= x], b ... ;`
        // What it cannot lay out goes to Decl::inexact; its members are kept (in
        // declaration order) but the layout is then not trusted.
        void MemberStatement(const std::string& cbName, std::vector<Member>& out)
        {
            const uint32_t line = Peek().line;
            bool rowMajor = false, colMajor = false, isStatic = false, isShared = false;
            while (Peek().kind == Tok::Ident && IsModifier(Peek().text, rowMajor, colMajor, isStatic, isShared)) Next();
            if (Peek().kind != Tok::Ident)
            {
                Inexact(line, "cbuffer '" + cbName + "': '" + Peek().text + "' where a member's type belongs");
                SkipStatement();
                return;
            }
            const std::string typeName = Peek().text;
            Next();
            TypeInfo type;
            if (IsP(Peek(), '<'))
            {
                Inexact(line, "cbuffer '" + cbName + "': a templated member type ('" + typeName + "<...>')");
                SkipBalanced('<', '>');
            }
            else if (typeName == "struct")
            {
                Inexact(line, "cbuffer '" + cbName + "': an inline struct member");
                SkipStatement();
                return;
            }
            else if (!NumericType(typeName, type))
            {
                const bool own = m_ctx.structs.count(typeName) != 0;
                const bool macro = m_ctx.macros.count(typeName) != 0;
                Inexact(line, "cbuffer '" + cbName + "': member type '" + typeName + "' " +
                              (own ? "is a struct" : macro ? "is a macro" : "is not a type the packer knows"));
            }
            else if (!type.known)
            {
                Inexact(line, "cbuffer '" + cbName + "': " + type.why);
            }
            if (isStatic) Inexact(line, "cbuffer '" + cbName + "': a static member");

            std::vector<size_t> mine;   // indices into out for this statement
            while (true)
            {
                Sync();
                if (Peek().kind != Tok::Ident)
                {
                    Inexact(Peek().line, "cbuffer '" + cbName + "': '" + Peek().text + "' where a member's name belongs");
                    SkipStatement();
                    return;
                }
                Member m;
                m.var.name = Peek().text;
                m.line     = Peek().line;
                m.type     = type;
                m.rowMajor = rowMajor;
                Next();
                bool plainName = true;   // nothing between the name and the ';' / ','
                std::vector<uint32_t> dims;
                bool unbounded = false;
                if (IsP(Peek(), '['))
                {
                    plainName = false;
                    ArraySuffix(dims, unbounded, m.line);
                    if (unbounded) Inexact(m.line, "cbuffer '" + cbName + "': '" + m.var.name + "[]' has no size");
                    if (dims.size() > 1) Inexact(m.line, "cbuffer '" + cbName + "': '" + m.var.name + "' is a multi-dimensional array");
                    if (dims.size() == 1) m.arrayLen = dims[0];
                }
                if (IsP(Peek(), ':'))
                {
                    plainName = false;
                    Next();
                    if (Peek().kind == Tok::Ident && Peek().text == "packoffset")
                        Inexact(m.line, "cbuffer '" + cbName + "': '" + m.var.name + "' has a packoffset");
                    else
                        Inexact(m.line, "cbuffer '" + cbName + "': '" + m.var.name + "' has a ':' binding");
                    while (Peek().kind != Tok::End && !IsP(Peek(), ',') && !IsP(Peek(), ';') && !IsP(Peek(), '='))
                    {
                        if (IsP(Peek(), '(')) { SkipBalanced('(', ')'); continue; }
                        Next();
                    }
                }
                if (IsP(Peek(), '='))
                {
                    plainName = false;   // StampSourceAnnotations finds no member name before the ';'
                    int depth = 0;
                    while (Peek().kind != Tok::End)
                    {
                        const Tok& t = Peek();
                        if (IsP(t, '(') || IsP(t, '{') || IsP(t, '[')) ++depth;
                        else if (IsP(t, ')') || IsP(t, '}') || IsP(t, ']')) --depth;
                        else if (depth <= 0 && (IsP(t, ',') || IsP(t, ';'))) break;
                        Next();
                    }
                }
                m.annotatable = plainName;
                mine.push_back(out.size());
                out.push_back(std::move(m));
                if (IsP(Peek(), ',')) { Next(); continue; }
                if (IsP(Peek(), ';'))
                {
                    const uint32_t semiLine = Peek().line;
                    // StampSourceAnnotations reads the identifier before the LAST
                    // ';' ahead of the marker: only this statement's last
                    // declarator, only when no other ';' follows on its line, and
                    // only when its name is on that line.
                    bool lastOnLine = true;
                    for (size_t k = m_ti + 1; k < m_lx.toks.size() && m_lx.toks[k].line == semiLine; ++k)
                        if (IsP(m_lx.toks[k], ';')) { lastOnLine = false; break; }
                    for (size_t k = 0; k < mine.size(); ++k)
                    {
                        Member& mm = out[mine[k]];
                        mm.semiLine = semiLine;
                        if (k + 1 != mine.size() || !lastOnLine || mm.line != semiLine) mm.annotatable = false;
                    }
                    Next();
                    break;
                }
                Inexact(Peek().line, "cbuffer '" + cbName + "': '" + Peek().text + "' after member '" +
                                     out.back().var.name + "'");
                SkipStatement();
                return;
            }
        }

        // A file-scope statement that is not a cbuffer / struct / typedef: a
        // resource, a global, a function, or something the parser cannot place.
        void Declaration()
        {
            const uint32_t line = Peek().line;
            const uint32_t depth = RealDepth();
            bool rowMajor = false, colMajor = false, isStatic = false, isShared = false;
            while (Peek().kind == Tok::Ident && IsModifier(Peek().text, rowMajor, colMajor, isStatic, isShared)) Next();
            if (Peek().kind != Tok::Ident) { SkipStatement(); return; }
            const std::string typeName = Peek().text;
            Next();
            if (IsP(Peek(), '<')) SkipBalanced('<', '>');
            if (IsP(Peek(), '('))
            {
                // NAME(...) at file scope: a macro that may declare anything.
                Inexact(line, "'" + typeName + "(...)' at file scope looks like a macro invocation");
                SkipBalanced('(', ')');
                if (IsP(Peek(), '{')) SkipBalanced('{', '}');
                else if (IsP(Peek(), ';')) Next();
                return;
            }
            if (Peek().kind != Tok::Ident) { SkipStatement(); return; }
            // A function: `type name(...) [: SEMANTIC] { ... }` or a prototype.
            if (IsP(Peek(1), '('))
            {
                Next();
                SkipBalanced('(', ')');
                while (IsP(Peek(), ':'))
                {
                    Next();
                    if (Peek().kind == Tok::Ident) Next();
                    if (IsP(Peek(), '(')) SkipBalanced('(', ')');
                }
                if (IsP(Peek(), '{')) SkipBalanced('{', '}');
                else if (IsP(Peek(), ';')) Next();
                return;
            }

            TypeInfo type;
            const bool resource = ResourceTypeOf(typeName, type);
            if (isStatic || isShared) { SkipStatement(); return; }   // not a resource, not in $Globals
            if (!resource)
            {
                Inexact(line, "'" + Peek().text + "' (" + typeName + "): a global outside any cbuffer "
                              "(it lands in $Globals)");
                SkipStatement();
                return;
            }

            bool first = true;
            while (true)
            {
                Sync();
                if (Peek().kind != Tok::Ident) { SkipStatement(); return; }
                ResourceBinding b;
                b.name = Peek().text;
                b.type = type.rtype;
                b.dim  = type.dim;
                const uint32_t nameLine = Peek().line;
                Next();
                std::vector<uint32_t> dims;
                bool unbounded = false;
                if (IsP(Peek(), '[')) ArraySuffix(dims, unbounded, nameLine);
                if (dims.size() > 1) Inexact(nameLine, "'" + b.name + "' is a multi-dimensional resource array");
                b.bindCount = unbounded ? 0u : (dims.empty() ? 1u : dims[0]);
                bool registered = false;
                if (IsP(Peek(), ':'))
                {
                    Next();
                    char cls = 0;
                    uint32_t idx = 0, space = 0;
                    if (Register(cls, idx, space, nameLine))
                    {
                        registered = true;
                        b.bindPoint = idx;
                        b.space     = space;
                        if (cls != type.regClass)
                            Inexact(nameLine, "'" + b.name + "' (" + typeName + ") at register class '" +
                                              std::string(1, cls) + "'");
                    }
                }
                if (!registered) Inexact(nameLine, "'" + b.name + "' has no literal register: DXC assigns one");
                if (depth > 0)   Inexact(nameLine, "'" + b.name + "' inside #if");
                if (type.templated) Inexact(nameLine, "'" + b.name + "': ConstantBuffer<T>'s layout is T's");
                // A texture's annotation: the marker on its line, after its ';'.
                if (first && b.type == ResourceType::Texture)
                {
                    const auto it = m_lx.notes.find(nameLine);
                    if (it != m_lx.notes.end() && !it->second.alone && it->second.semiBefore)
                        ForEachSection(it->second.body,
                                       [&](const std::string& sec) { ApplyTextureAnnoSection(sec, b, m_path); });
                }
                m_ctx.decl.sites.push_back({ b.name, m_path, nameLine });
                m_ctx.decl.iface.bindings.push_back(std::move(b));
                first = false;
                if (IsP(Peek(), ',')) { Next(); continue; }
                if (IsP(Peek(), ';')) { Next(); return; }
                SkipStatement();
                return;
            }
        }

        Ctx&               m_ctx;
        const std::string  m_path;
        const Lexed&       m_lx;
        size_t             m_ti = 0;     // the current token
        size_t             m_di = 0;     // the next directive to process
        // #if nesting: true = an include guard (#ifndef X + #define X), transparent.
        std::vector<bool>  m_conds;
        bool               m_inCBuffer = false;
    };

    bool DefaultRead(const std::string& path, std::string& text)
    {
        std::vector<uint8_t> buf;
        if (!Resource::AssetFS::Get().ReadFile(path, buf) || buf.empty()) return false;
        text.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
        return true;
    }

    const char* TypeName(ResourceType t) { return ShaderReflect::ToString(t); }

    std::string Reg(const ResourceBinding& b)
    {
        char c = 't';
        switch (b.type)
        {
        case ResourceType::Sampler:            c = 's'; break;
        case ResourceType::CBuffer:            c = 'b'; break;
        case ResourceType::RWTexture:
        case ResourceType::RWStructuredBuffer: c = 'u'; break;
        default: break;
        }
        return std::string(1, c) + std::to_string(b.bindPoint) + " space" + std::to_string(b.space);
    }

    std::string Floats(const std::array<float, 4>& v)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%g %g %g %g", v[0], v[1], v[2], v[3]);
        return buf;
    }
} // anonymous namespace

// ==== Public API ====
std::string ResolvePath(const std::string& f, const std::string& shaderDir)
{
    const bool isAbsolute = (f.size() >= 2 && f[1] == ':') ||
                            (f.size() >= 2 && f[0] == '\\' && f[1] == '\\') ||
                            (!f.empty() && f[0] == '/');
    const bool hasDir = f.find('/') != std::string::npos || f.find('\\') != std::string::npos;
    return (isAbsolute || hasDir) ? f : (shaderDir + f);
}

Decl Parse(const std::string& rootPath, const std::string& shaderDir, const ReadFn& read)
{
    Decl decl;
    decl.root = Normal(rootPath);
    const ReadFn useRead = read ? read : ReadFn(DefaultRead);
    std::string text;
    if (!useRead(rootPath, text)) return decl;
    decl.read = true;
    Ctx ctx{ decl, shaderDir, useRead, {}, {}, {} };
    FileParser::ParseFile(ctx, rootPath, text);
    for (const std::string& f : decl.files) decl.fileTimes.push_back(ShaderDiskCache::FileWriteTime(f));
    return decl;
}

bool Stale(const Decl& decl)
{
    for (size_t i = 0; i < decl.files.size() && i < decl.fileTimes.size(); ++i)
        if (ShaderDiskCache::FileWriteTime(decl.files[i]) != decl.fileTimes[i]) return true;
    return false;
}

std::shared_ptr<const Decl> Get(const std::string& hlslFile, const std::string& shaderDir)
{
    static std::mutex                                                   s_mutex;
    static std::unordered_map<std::string, std::shared_ptr<const Decl>> s_cache;

    const std::string path = ResolvePath(hlslFile, shaderDir);
    const std::string key  = path + "|" + shaderDir;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(key);
    // An unreadable root has no files to stamp: parsed again every time, so a
    // file that appears is seen (the caller caches per reload epoch).
    if (it != s_cache.end() && it->second->read && !Stale(*it->second)) return it->second;
    auto decl = std::make_shared<const Decl>(Parse(path, shaderDir));
    s_cache[key] = decl;
    return decl;
}

std::vector<std::string> CrossCheck(const Decl& decl, const Reflection& refl)
{
    std::vector<std::string> out;
    // An inexact parse is not an interface anyone reads (the DXIL's is kept):
    // there is nothing to hold it to.
    if (!decl.Exact()) return out;
    auto site = [&](const std::string& name) -> std::string {
        for (const Site& s : decl.sites)
            if (s.name == name) return s.file + ":" + std::to_string(s.line) + ": ";
        return std::string();
    };
    // The stamp reads the root's text only: an own include's annotations are
    // the declaration's alone, nothing to compare them with.
    auto inRoot = [&](const std::string& name) {
        for (const Site& s : decl.sites)
            if (s.name == name) return s.file == decl.root;
        return false;
    };

    // The engine reads gTimeParams only through `p.time` / `v.time`, so a variant that reads it
    // reads the material's own time. One way only: a variant may compile the time away.
    if (refl.readsFrameTime && !decl.iface.readsFrameTime)
        out.push_back(decl.root + ": the DXIL reads the frame time (gTimeParams), the source shows no "
                      "`.time` or gTimeParams");

    for (const ResourceBinding& r : refl.bindings)
    {
        const auto d = std::find_if(decl.iface.bindings.begin(), decl.iface.bindings.end(),
                                    [&](const ResourceBinding& b) { return b.name == r.name; });
        if (d == decl.iface.bindings.end()) continue;   // the engine's (or the parser missed it)
        const std::string at = site(r.name) + "'" + r.name + "': ";
        if (d->type != r.type)
        {
            out.push_back(at + "declared a " + TypeName(d->type) + ", the DXIL has a " + TypeName(r.type));
            continue;
        }
        if (d->bindPoint != r.bindPoint || d->space != r.space)
            out.push_back(at + "declared at " + Reg(*d) + ", the DXIL binds " + Reg(r));
        if (d->bindCount != r.bindCount)
            out.push_back(at + "declared with " + std::to_string(d->bindCount) + " descriptor(s), the DXIL has " +
                          std::to_string(r.bindCount));
        if ((d->type == ResourceType::Texture || d->type == ResourceType::RWTexture) && d->dim != r.dim)
            out.push_back(at + "declared as " + ShaderReflect::ToString(d->dim) + ", the DXIL has " +
                          ShaderReflect::ToString(r.dim));
        if (r.type == ResourceType::Texture && inRoot(r.name) &&
            (d->texDefault != r.texDefault || d->colorSpace != r.colorSpace || d->engineRole != r.engineRole))
            out.push_back(at + "the annotations differ (default / colour space / @engine)");
    }

    for (const CBufferLayout& c : refl.cbuffers)
    {
        const auto d = std::find_if(decl.iface.cbuffers.begin(), decl.iface.cbuffers.end(),
                                    [&](const CBufferLayout& x) { return x.name == c.name; });
        if (d == decl.iface.cbuffers.end()) continue;
        const std::string at = site(c.name) + "cbuffer '" + c.name + "': ";
        if (d->sizeBytes != c.sizeBytes)
            out.push_back(at + std::to_string(d->sizeBytes) + " bytes by the packing rules, " +
                          std::to_string(c.sizeBytes) + " in the DXIL");
        if (d->vars.size() != c.vars.size())
        {
            std::string a, b;
            for (const CBufferVar& v : d->vars) a += (a.empty() ? "" : " ") + v.name;
            for (const CBufferVar& v : c.vars)  b += (b.empty() ? "" : " ") + v.name;
            out.push_back(at + std::to_string(d->vars.size()) + " members declared (" + a + "), " +
                          std::to_string(c.vars.size()) + " in the DXIL (" + b + ")");
        }
        const size_t n = std::min(d->vars.size(), c.vars.size());
        for (size_t i = 0; i < n; ++i)
        {
            const CBufferVar& x = d->vars[i];
            const CBufferVar& y = c.vars[i];
            const std::string m = site(c.name + "." + x.name) + "cbuffer '" + c.name + "' member " + std::to_string(i) +
                                  " '" + x.name + "': ";
            if (x.name != y.name) { out.push_back(m + "the DXIL's is '" + y.name + "'"); continue; }
            if (x.type != y.type)
                out.push_back(m + "type " + ShaderReflect::ToString(x.type) + ", the DXIL's " + ShaderReflect::ToString(y.type));
            if (x.offset != y.offset || x.size != y.size || x.elements != y.elements)
                out.push_back(m + "offset " + std::to_string(x.offset) + " size " + std::to_string(x.size) +
                              " elements " + std::to_string(x.elements) + " by the packing rules, the DXIL's offset " +
                              std::to_string(y.offset) + " size " + std::to_string(y.size) + " elements " +
                              std::to_string(y.elements));
            if (!inRoot(c.name + "." + x.name)) continue;
            if (x.hasDefault != y.hasDefault || (x.hasDefault && x.defaultValue != y.defaultValue))
                out.push_back(m + "@default " + (x.hasDefault ? Floats(x.defaultValue) : "none") + ", stamped " +
                              (y.hasDefault ? Floats(y.defaultValue) : "none"));
            if (x.hasRange != y.hasRange || (x.hasRange && (x.minValue != y.minValue || x.maxValue != y.maxValue)))
                out.push_back(m + "@min / @max differ");
            if (x.label != y.label || x.group != y.group || x.engineRole != y.engineRole)
                out.push_back(m + "@label / @group / @engine differ ('" + x.label + "' '" + x.group + "' '" + x.engineRole +
                              "' vs '" + y.label + "' '" + y.group + "' '" + y.engineRole + "')");
        }
    }

    if (decl.rootEngineRolesExplicit != refl.engineRolesExplicit)
        out.push_back(std::string("engineRolesExplicit: ") + (decl.rootEngineRolesExplicit ? "true" : "false") +
                      " read from the root, " + (refl.engineRolesExplicit ? "true" : "false") + " stamped");
    return out;
}

void StampSourceAnnotations(const std::string& src, Reflection& refl, const std::string& file)
{
    if (refl.cbuffers.empty() && refl.bindings.empty()) return;
    std::istringstream ss(src);
    std::string line;
    while (std::getline(ss, line))
    {
        const size_t at = line.find("//@");
        if (at == std::string::npos) continue;

        // `//@engine explicit` on a line of its own: the shader reads no engine role from a positional slot.
        // For a shader with no role to name, such as a material graph's Toon material, which has no ramp
        // texture and so need not keep t3 free.
        if (line.find_first_not_of(" \t") == at)
        {
            if (EngineExplicitLine(line.substr(at + 3), file)) refl.engineRolesExplicit = true;
            continue;
        }

        // Member name = the identifier immediately before the ';' that precedes
        // the comment (e.g. "    float4 FaceShadow;  //@default ...").
        CBufferVar*      var = nullptr;
        ResourceBinding* tex = nullptr;
        const size_t semi = line.rfind(';', at);
        if (semi == std::string::npos) continue;
        // A declaration that names an engine role makes the whole shader explicit - checked before the
        // lookup, because DXC strips a texture the shader never samples and its annotation still counts.
        if (line.find("@engine", at) != std::string::npos) refl.engineRolesExplicit = true;
        const std::string name = IdentifierBefore(line, semi);
        for (auto& cb : refl.cbuffers)
            for (auto& v : cb.vars)
                if (!name.empty() && v.name == name) { var = &v; break; }
        if (!var)
        {
            // A texture: `Texture2D _Mask : register(t2, space3); //@default black`
            // - the name is the identifier before the register colon (or the
            // ';' when there is none), past an array suffix.
            const std::string code = line.substr(0, at);
            size_t end = code.find(':');
            if (end == std::string::npos) end = code.find(';');
            if (end == std::string::npos) continue;
            const size_t bracket = code.rfind('[', end);
            if (bracket != std::string::npos && code.find(']', bracket) < end) end = bracket;
            const std::string texName = IdentifierBefore(code, end);
            for (auto& b : refl.bindings)
                if (!texName.empty() && b.name == texName &&
                    b.type == ShaderReflect::ResourceType::Texture) { tex = &b; break; }
            if (!tex) continue;
        }

        ForEachSection(line.substr(at + 3), [&](const std::string& sec) {
            if (var) ApplyAnnoSection(sec, *var, file);
            else     ApplyTextureAnnoSection(sec, *tex, file);
        });
    }
}

} // namespace MaterialShaderDecl
