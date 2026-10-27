#pragma once

// LayoutCheck - checks a C++ struct against its HLSL mirror on the DXC reflection of every shader
// the engine loads.
// Why: constant buffers and structured-buffer elements are written by C++ and read by HLSL through
// hand-kept mirrors (PerViewCB is declared in many shaders under several naming schemes). A member
// that moves on one side reads the wrong bytes on the other with no compile error or D3D12 message;
// the static_asserts beside the C++ structs pin only that side.
// The check: a Spec lists the C++ members - HLSL name(s), offsetof, sizeof (LAYOUT_VAR). A reflected
// block a registered spec claims BY NAME is compared member by member:
//   * a member whose HLSL name the spec lists (any alias) must sit at the C++ offset with the C++ size;
//   * an unlisted member must still COINCIDE with some C++ member (offset and size): position is what
//     the GPU reads;
//   * a member whose name starts with '_' is padding: it only has to lie inside the C++ struct;
//   * a cbuffer may be SHORTER than the struct (a shader declares the prefix it reads), never longer;
//     a structured-buffer element's stride must equal sizeof.
// A cbuffer member's size is its reflected size; for an array that is the whole array as the cbuffer
// packs it (each element in its own 16-byte row), which is how `float x[4]` against a C++ float[4] is
// caught.
// Runs in ShaderLibrary::GetShaderInternal and ShaderDiskCache::LoadOrCompile, so every shader is
// checked on first load and on every hot reload. A mismatch is an ERROR once per (block, shader,
// member) per process, so a harness run with -maxerrors=0 fails on it.
// Proof of life: "[LAYOUT] <block> (<C++ type>, N B) first checked against <shader>: M members, K
// mismatches" once per spec per process, so "0 mismatches" and "never checked" differ in a log.

#include <cstddef>
#include <cstdint>

namespace ShaderReflect { struct Reflection; }

namespace Layout
{
    struct Var
    {
        const char* names;    // the HLSL name(s), '|'-separated aliases
        uint32_t    offset;   // offsetof in the C++ struct
        uint32_t    size;     // sizeof the C++ member
    };

    enum class Kind : uint8_t
    {
        CBuffer,         // a `cbuffer <block>`
        StructElement,   // StructuredBuffer<T>: matched by the element TYPE name
    };

    struct Spec
    {
        const char* block;            // cbuffer name, or the element struct's HLSL name
        Kind        kind;
        const char* cppType;          // for messages
        uint32_t    cppSize;          // sizeof the C++ struct
        const Var*  vars;
        uint32_t    count;
        // '|'-separated HLSL member names that mark a same-named block as ANOTHER struct (WorldUI's own
        // `cbuffer PerViewCB`): such a block is skipped, not reported. nullptr = none.
        const char* foreign = nullptr;
    };

    struct Result
    {
        uint32_t blocks     = 0;   // blocks compared
        uint32_t vars       = 0;   // members compared
        uint32_t mismatches = 0;
    };

    // Sets the specs every loaded shader is checked against, claimed by block name. Call once before
    // the first shader load (the Renderer constructor does); a later call replaces the table. The table
    // must outlive every load.
    void Register(const Spec* specs, uint32_t count);

    // Checks @p r against every registered spec that names one of its blocks; @p shader labels the
    // messages.
    Result CheckRegistered(const char* shader, const ShaderReflect::Reflection& r);

    // Logs "[LAYOUT] summary: specs= mismatches= shaders: <block>=<n> ...": how many distinct shaders
    // each registered spec was checked against so far. The Renderer logs it at shutdown, so a run states
    // its coverage.
    void LogSummary();

    // Checks one spec against one reflection, regardless of the registry: for a block whose name is not
    // unique engine-wide, checked where its owner loads it. A missing block counts as a mismatch.
    Result Check(const char* shader, const ShaderReflect::Reflection& r, const Spec& spec);
}

// One member: LAYOUT_VAR("viewProj|g_viewProj", Renderer::PerViewCB, viewProj).
#define LAYOUT_VAR(hlslNames, Type, member) \
    ::Layout::Var{ hlslNames, static_cast<uint32_t>(offsetof(Type, member)), \
                   static_cast<uint32_t>(sizeof(Type::member)) }

// A spec over a static Var array: LAYOUT_SPEC("LightCB", CBuffer, Renderer::LightCB, kLightCB).
#define LAYOUT_SPEC(block, kind, Type, vars) \
    ::Layout::Spec{ block, ::Layout::Kind::kind, #Type, static_cast<uint32_t>(sizeof(Type)), \
                    vars, static_cast<uint32_t>(sizeof(vars) / sizeof((vars)[0])) }
