#pragma once
// MaterialShaderDecl.h - what a material shader's OWN source declares, read from the text before
// anything is compiled.
// A material shader is user source registered at runtime (DynamicShaderRegistry): a custom surface /
// vertex shader, a material graph's generated .hlsl, a material sky CS, a trail / UI / particle
// shader. Its OWN files are the root and whatever it includes from outside the engine shader root
// (shaders/); the engine headers it includes are the engine's interface, not the material's, and are
// never parsed for declarations.
// Parse() TOKENIZES those files - comments, strings and preprocessor lines are tokens, not a text
// search, so a name in a comment declares nothing - and records:
//   - capabilities: #define MATERIAL_HAS_SURFACE / _VERTEX_OFFSET / _VERTEX_MODIFY
//   - the domain: #define MATERIAL_DOMAIN_WATER (none = surface)
//   - the permutation defines the own files name (PermutationKey bits)
//   - every resource they declare - cbuffers with their members laid out by the HLSL cbuffer packing
//     rules, textures, samplers, buffers - with the //@ annotations of the declaration's line
// in a ShaderReflect::Reflection, the shape the DXIL reflection has, so a consumer can read either and
// CrossCheck() can compare them.
// What it cannot read exactly - a declaration inside #if, a type it does not know (a struct, a macro,
// half / double), a register or array size that is not a literal, packoffset, a global outside any
// cbuffer (it lands in $Globals) - is listed in Decl::inexact ("file:line: why"); a consumer then
// keeps the DXIL reflection for that shader. The interface never silently loses a member.

#include "Graphics/ShaderReflection.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace MaterialShaderDecl
{
    enum class Domain : uint8_t { Surface, Water };

    // Where one declaration is: a binding (name = its name) or a cbuffer member
    // (name = "<cbuffer>.<member>").
    struct Site
    {
        std::string name;
        std::string file;
        uint32_t    line = 0;
    };

    struct Decl
    {
        bool                     read = false;   // the root source was readable
        std::string              root;           // the resolved root path
        std::vector<std::string> files;          // the own files parsed, root first
        std::vector<uint64_t>    fileTimes;      // their write times at the parse (0: not on disk, a pak)

        // #define MATERIAL_HAS_* in an own file (a later #undef clears it).
        bool hasSurface      = false;
        bool hasVertexOffset = false;
        bool hasVertexModify = false;
        Domain   domain   = Domain::Surface;     // #define MATERIAL_DOMAIN_WATER
        // The PermutationKey bits whose defines the own files name in code or in
        // a directive (comments excluded); ~0u when they paste tokens (##).
        uint32_t permBits = 0;
        // The engine sources the own files #include, as paths under the shader root
        // ("shaders/custom_material.hlsli"); with permBits, what a dynamic shader's permutation mask is made
        // of (ShaderLibrary::DynamicPermMask).
        std::vector<std::string> engineIncludes;
        // The includes, as written, that are neither an own file nor a file under
        // the shader root - tools/gen_perm_relevance.py's "unresolved include". One
        // that leaves the root ("../TextFX.hlsli") may still compile, DxcCompiler's
        // include handler finding it by its base name; the mask keeps every bit.
        std::vector<std::string> unresolvedIncludes;

        // bindings: every resource declared, in declaration order, with its
        // register as written; cbuffers: each cbuffer's members laid out by the
        // packing rules (`used` is true for all: the source cannot know), sizeBytes
        // rounded up to 16 as DXC reports it; annotations stamped as on a
        // reflection; engineRolesExplicit from any own file; readsFrameTime when an own file's code
        // reads `.time` or gTimeParams.
        ShaderReflect::Reflection iface;
        std::vector<Site>         sites;      // one per binding and per cbuffer member
        // engineRolesExplicit from the root file alone - what ShaderLibrary's
        // stamp of the root source gives a reflection (CrossCheck compares this).
        bool                      rootEngineRolesExplicit = false;

        std::vector<std::string>  inexact;    // "file:line: why"; non-empty = keep the DXIL reflection
        std::vector<std::string>  notes;      // the same, for what does not change the interface
        // "file:line: why" for what the author almost certainly did not mean: a permutation define tested
        // with #ifdef / #ifndef / defined() alone - every one is passed as 0 or 1, so the test is always
        // true.
        std::vector<std::string>  warnings;

        bool Exact() const { return read && inexact.empty(); }
    };

    using ReadFn = std::function<bool(const std::string& path, std::string& text)>;

    // @p hlslFile resolved as ShaderLibrary::LoadOrCompile resolves it: a path
    // with a directory (or a drive / rooted path) as given, a bare file name under
    // @p shaderDir.
    std::string ResolvePath(const std::string& hlslFile, const std::string& shaderDir);

    // Parse the own files of @p rootPath (resolved). @p read defaults to AssetFS,
    // so a packed build reads the .hlsl the package carries.
    Decl Parse(const std::string& rootPath, const std::string& shaderDir, const ReadFn& read = {});

    // The parse of @p hlslFile (ResolvePath), cached per process and parsed again
    // when one of its own files changed on disk (a hot-reloaded edit). Never null.
    // Thread-safe.
    std::shared_ptr<const Decl> Get(const std::string& hlslFile, const std::string& shaderDir);

    // An own file of @p decl was written since it was parsed (Get would parse it
    // again). A file that is not on disk (a packed build) never is.
    bool Stale(const Decl& decl);

    // Where a compiled variant's DXIL reflection disagrees with the declarations:
    // one line per difference, empty when they agree. Only what the own files
    // declare is compared, and only where the variant kept it (DXC strips a
    // resource the variant never reads): a binding's type, register, space,
    // count and (textures) dimension; a cbuffer's register, size and every
    // member's name, type, offset, size and element count in order; the
    // annotations of what the ROOT declares (the reflection is stamped from the
    // root's text alone, so an own include's annotations are the declaration's
    // only); engineRolesExplicit against the root's. Nothing for an inexact
    // declaration: no one reads it.
    std::vector<std::string> CrossCheck(const Decl& decl, const ShaderReflect::Reflection& refl);

    // The //@ annotations of @p src (one file's text) stamped onto @p refl by
    // member / texture name: `//@default / @min / @max / @label / @group / @engine`
    // after a declaration's ';' on its line, `//@engine explicit` on a line of its
    // own. ShaderLibrary stamps every compiled shader's reflection with its root
    // source through this; the parser reads the same grammar (one copy of it).
    void StampSourceAnnotations(const std::string& src, ShaderReflect::Reflection& refl,
                                const std::string& file);
}
