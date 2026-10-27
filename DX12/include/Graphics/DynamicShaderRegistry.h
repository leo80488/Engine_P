#pragma once
// DynamicShaderRegistry.h - every runtime-registered shader in one table: a custom
// material's PS / VS, a particle / trail / UI / world-UI billboard shader, the
// material sky CS. Every ShaderLibrary resolves a dynamic id here, so a shader is
// registered once and any pass library can compile it, with no per-library copy.
//
// The id is a function of the shader, not of when it was first seen:
// kIdBase | (FNV-1a("path|entry|stage") & kIdMask). A new static ShaderID moves no
// dynamic id, and PSO library names do not follow the scene's load order. kIdBase
// puts every id in [2^30, 2^31): above any static ShaderID, and still positive as
// an int (the renderer's resolve state uses -1 / 0 as sentinels).
//
// A collision (two keys, one hash) takes the next free id with a warning - the only
// case where an id depends on registration order.
//
// Entries are immutable once inserted and live for the process, so a compile
// worker may hold an Entry* while another thread registers. Register and the
// lookups are thread-safe. The one mutable field is the cached permutation key mask
// (atomic): ShaderLibrary computes it on first use from the declarations and a hot
// reload forgets it.

#include "Graphics/GraphicsStruct.h"   // RHI::ShaderStage

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// What a library needs to compile a shader: the source, the entry point, the
// stage. A static ShaderID's slot and a dynamic entry share this shape.
struct ShaderSourceEntry
{
    std::string      hlslFile;
    std::string      entry;
    RHI::ShaderStage stage = RHI::ShaderStage::Count;
    bool             registered = false;
};

class DynamicShaderRegistry
{
public:
    struct Entry : ShaderSourceEntry
    {
        uint32_t    id = 0;
        std::string name;   // the file name, for logs ("EndfieldBody.ps.hlsl")
        // The PermutationKey bits the shader is cached under (ShaderLibrary::KeyBits):
        // bit 32 set = known, the low 32 bits the mask. 0 = not computed yet
        // (ShaderLibrary computes it on first use).
        mutable std::atomic<uint64_t> keyMask{ 0 };
    };

    static constexpr uint32_t kIdBase    = 1u << 30;
    static constexpr uint32_t kIdMask    = kIdBase - 1u;
    static constexpr uint32_t kInvalidId = ~0u;   // == ShaderLibrary::kInvalidDynShaderID

    // The process's one registry.
    static DynamicShaderRegistry& Get();

    static bool IsDynamicId(uint32_t id) { return id >= kIdBase && id < (kIdBase << 1); }

    // The id of (hlslFile, entry, stage): registered on first sight, the same id
    // ever after. kInvalidId for an empty path.
    uint32_t Register(const char* hlslFile, RHI::ShaderStage stage, const char* entry = "main");
    // The id a Register already gave this triple, or kInvalidId. Never inserts.
    uint32_t Find(const char* hlslFile, RHI::ShaderStage stage, const char* entry = "main") const;
    // The entry behind @p id, or nullptr. Stable for the process.
    const Entry* Lookup(uint32_t id) const;
    size_t Size() const;

    // Every entry's cached key mask is computed again on its next use (a hot
    // reload: an edited source may name other bits).
    void ForgetKeyMasks() const;

    // For a log line: a dynamic id's file name, any other id as its number
    // ("1", "0"), an unknown dynamic id as "#<id>".
    std::string Label(uint32_t id) const;

    // The registry key: "path|entry|stage" (stage as one digit).
    static std::string MakeKey(const char* hlslFile, const char* entry, RHI::ShaderStage stage);
    // kIdBase | (FNV-1a 32 of @p key & kIdMask): the id when nothing collides.
    static uint32_t HashId(const std::string& key);

private:
    DynamicShaderRegistry() = default;

    mutable std::shared_mutex                 m_mutex;
    std::vector<std::unique_ptr<Entry>>       m_entries;   // stable addresses
    std::unordered_map<std::string, uint32_t> m_keyToId;
    std::unordered_map<uint32_t, const Entry*> m_idToEntry;
};
