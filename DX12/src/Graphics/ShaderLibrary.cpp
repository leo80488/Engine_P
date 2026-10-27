#include <set>
#include "Graphics/ShaderBindingValidation.h"
#include "Graphics/LayoutCheck.h"
#include "Graphics/MaterialShaderDecl.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PermutationRelevance.generated.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/ShaderRecipes.h"
#include "Graphics/PipelineStats.h"   // shaderMisses
#include "Diagnostics/Profiler.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cassert>
#include <unordered_set>

// ==== Helpers ====
namespace
{
    // The root source a reflection's //@ annotations are stamped from, read once per write time
    // and shared by every library that misses a variant of it, on any thread. A file the disk
    // cannot date (a pak entry: write time 0) is read each time.
    std::shared_ptr<const std::string> CachedSource(const std::string& hlslPath)
    {
        struct Entry { uint64_t time = 0; std::shared_ptr<const std::string> text; };
        static std::mutex                             s_mutex;
        static std::unordered_map<std::string, Entry> s_entries;
        const uint64_t time = ShaderDiskCache::FileWriteTime(hlslPath);
        if (time != 0)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            const auto it = s_entries.find(hlslPath);
            if (it != s_entries.end() && it->second.time == time) return it->second.text;
        }
        std::vector<uint8_t> src;
        ::Resource::AssetFS::Get().ReadFile(hlslPath, src);
        if (src.empty()) return nullptr;
        auto text = std::make_shared<const std::string>(reinterpret_cast<const char*>(src.data()), src.size());
        if (time != 0)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_entries[hlslPath] = Entry{ time, text };
        }
        return text;
    }

    // ==== Relevant-permutation mask table ====
    // The per-ShaderID half of the shader registry: which PermutationKey bits can change a
    // shader's bytecode. A bit belongs in the mask only if the .hlsl source or its transitive
    // #include closure names the matching define (bit -> define map: kPermutationDefines).
    // Generated (Graphics/PermutationRelevance.generated.h, tools/gen_perm_relevance.py): every
    // id src/ registers with a literal file gets the bits its closure names, comments stripped;
    // an id without a row keeps ~0u. A mask that is too narrow is a correctness bug - two
    // different variants would share one .ishdr / one PSO and half of them would run the wrong
    // bytecode - which is why the table is generated from the sources, why verify's
    // perm_relevance gate fails while it is stale, and why LoadOrCompile checks the real closure
    // of each source version it loads against it (CheckRelevance).
    constexpr std::array<uint32_t, static_cast<size_t>(ShaderID::Count)>
    BuildRelevantPermMaskTable()
    {
        std::array<uint32_t, static_cast<size_t>(ShaderID::Count)> t{};
        for (uint32_t& m : t) m = ~0u;
        for (const PermutationRelevance::Row& row : PermutationRelevance::kRows)
            t[static_cast<size_t>(row.id)] = row.mask;
        return t;
    }

    constexpr auto kRelevantPermMask = BuildRelevantPermMaskTable();

    // ==== The generated table checked at load ====
    // The same rules as tools/gen_perm_relevance.py: comments stripped, a define counts when its
    // name appears as a whole word, token pasting makes every bit count.
    std::string StripComments(const char* p, size_t n)
    {
        std::string out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (p[i] == '/' && i + 1 < n && p[i + 1] == '/')
            {
                while (i < n && p[i] != '\n') ++i;
                out.push_back('\n');
            }
            else if (p[i] == '/' && i + 1 < n && p[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < n && !(p[i] == '*' && p[i + 1] == '/')) ++i;
                ++i;
                out.push_back(' ');
            }
            else
                out.push_back(p[i]);
        }
        return out;
    }

    bool NamesWord(const std::string& text, const char* word)
    {
        auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        const size_t len = std::strlen(word);
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + len))
            if ((at == 0 || !isIdent(text[at - 1])) && (at + len >= text.size() || !isIdent(text[at + len])))
                return true;
        return false;
    }

    uint32_t NamedPermBits(const std::string& text)
    {
        if (text.find("##") != std::string::npos) return ~0u;
        uint32_t bits = 0;
        for (const PermutationDefine& d : kPermutationDefines)
            if (NamesWord(text, d.name)) bits |= d.bit;
        return bits;
    }

    // One file, scanned once per write time: the permutation bits it names, the
    // shader resources it declares, and its #include lines, resolved the way
    // the generator (and ShaderDiskCache) resolve them - a quoted include next
    // to the file, then under the shader root; an angle include under the root
    // only. An unresolved .hlsl / .hlsli include names every bit (the generator
    // leaves such a shader no row); a C++ standard header in a __cplusplus
    // branch is not HLSL.
    struct FileScan
    {
        uint64_t                 time = 0;
        uint32_t                 bits = 0;
        std::vector<std::string> includes;
        std::vector<std::string> declared;   // cbuffer / ConstantBuffer / texture / buffer names
    };

    // The resource names a file's (comment-stripped) text declares. A function
    // parameter of a texture type matches too; it names no reflected binding.
    void DeclaredResources(const std::string& text, std::vector<std::string>& out)
    {
        static const std::regex kDecl(
            R"(\bcbuffer\s+(\w+)|\bConstantBuffer\s*<[^>;]*>\s*(\w+)|)"
            R"(\b(?:RW)?(?:Texture(?:1D|2D|3D|Cube)(?:Array)?|Texture2DMS(?:Array)?|StructuredBuffer|ByteAddressBuffer|Buffer))"
            R"(\s*(?:<[^>;]*>)?\s+(\w+))");
        for (std::sregex_iterator it(text.begin(), text.end(), kDecl), end; it != end; ++it)
            for (size_t g = 1; g <= 3; ++g)
                if ((*it)[g].matched) { out.push_back((*it)[g].str()); break; }
    }

    void ScanFileText(const std::string& path, const std::string& shaderDir, FileScan& s)
    {
        std::ifstream f(Sys::U8Path(path), std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const std::string text = StripComments(raw.data(), raw.size());
        s.bits = NamedPermBits(text);
        DeclaredResources(text, s.declared);
        const std::filesystem::path dir = Sys::U8Path(path).parent_path();
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);)
        {
            size_t i = line.find_first_not_of(" \t");
            if (i == std::string::npos || line[i] != '#') continue;
            i = line.find_first_not_of(" \t", i + 1);
            if (i == std::string::npos || line.compare(i, 7, "include") != 0) continue;
            i = line.find_first_not_of(" \t", i + 7);
            if (i == std::string::npos || (line[i] != '"' && line[i] != '<')) continue;
            const bool angle = line[i] == '<';
            const size_t end = line.find(angle ? '>' : '"', i + 1);
            if (end == std::string::npos) continue;
            const std::string name = line.substr(i + 1, end - i - 1);
            std::error_code ec;
            std::filesystem::path p = angle ? Sys::U8Path(shaderDir) / Sys::U8Path(name) : dir / Sys::U8Path(name);
            if (!angle && !std::filesystem::exists(p, ec)) p = Sys::U8Path(shaderDir) / Sys::U8Path(name);
            if (std::filesystem::exists(p, ec))
                s.includes.push_back(Sys::PathToUtf8(p.lexically_normal()));
            else if (name.find('/') != std::string::npos || name.ends_with(".hlsl") || name.ends_with(".hlsli"))
                s.bits = ~0u;
        }
    }

    // What the #include closure of @p root says, from the files on disk (a
    // packed build has none: everything stays empty).
    struct ClosureInfo
    {
        uint32_t              bits = 0;          // permutation bits the closure names
        bool                  surfaceMaterial = false;   // includes custom_surface / custom_vertex.hlsli
        std::set<std::string> ownDeclared;       // resources declared OUTSIDE the engine shader root
    };

    // Whether @p path (as the closure walk spells it) lies under @p dir.
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

    ClosureInfo ScanClosure(const std::string& root, const std::string& shaderDir)
    {
        static std::mutex s_mutex;
        static std::unordered_map<std::string, FileScan> s_scans;
        std::lock_guard<std::mutex> lock(s_mutex);
        ClosureInfo info;
        std::vector<std::string> stack{ Sys::PathToUtf8(Sys::U8Path(root).lexically_normal()) };
        std::set<std::string> seen;
        while (!stack.empty())
        {
            const std::string path = std::move(stack.back());
            stack.pop_back();
            if (!seen.insert(path).second) continue;
            const uint64_t time = ShaderDiskCache::FileWriteTime(path);
            if (time == 0) continue;
            FileScan& s = s_scans[path];
            if (s.time != time)
            {
                s = FileScan{};
                s.time = time;
                ScanFileText(path, shaderDir, s);
            }
            info.bits |= s.bits;
            const std::string file = Sys::PathToUtf8(Sys::U8Path(path).filename());
            if (file == "custom_surface.hlsli" || file == "custom_vertex.hlsli") info.surfaceMaterial = true;
            if (!IsUnder(path, shaderDir)) info.ownDeclared.insert(s.declared.begin(), s.declared.end());
            stack.insert(stack.end(), s.includes.begin(), s.includes.end());
        }
        return info;
    }

    // The bits the #include closure of @p root names, or 0 when @p root is not
    // on disk (a packed build: nothing to compare against).
    uint32_t ClosureBits(const std::string& root, const std::string& shaderDir)
    {
        return ScanClosure(root, shaderDir).bits;
    }

    // Process-wide, like the table: closures checked, and those that named a bit
    // their row lacks (ShaderLibrary::LogRelevanceSummary).
    std::atomic<uint32_t> g_relChecked{ 0 };
    std::atomic<uint32_t> g_relStale{ 0 };
    std::atomic<uint32_t> g_relDynamic{ 0 };   // of them, dynamic shaders

    // The shader root every library compiles against (each Init passes "shaders/"). KeyBits is
    // static, so the dynamic key mask cannot ask a library for it.
    constexpr const char* kEngineShaderDir = "shaders/";

    // A dynamic shader's key mask, cached on its registry entry: computed on first
    // use from the declarations (ShaderLibrary::DynamicPermMask), forgotten by a
    // hot reload (ClearCaches). Every bit for an id the registry does not know.
    uint32_t DynamicKeyMask(uint32_t id) noexcept
    {
        const DynamicShaderRegistry::Entry* e = DynamicShaderRegistry::Get().Lookup(id);
        if (!e) return ~0u;
        const uint64_t cached = e->keyMask.load(std::memory_order_acquire);
        if (cached >> 32) return static_cast<uint32_t>(cached);
        uint32_t mask = ~0u;
        try
        {
            mask = ShaderLibrary::DynamicPermMask(*MaterialShaderDecl::Get(e->hlslFile, kEngineShaderDir));
        }
        catch (...) {}   // a parse that throws keeps every bit
        e->keyMask.store((uint64_t{ 1 } << 32) | mask, std::memory_order_release);
        return mask;
    }

    // -permrel_break=<file> (harness): the check reads the row of every shader
    // whose source path ends in <file> as 0, so a closure that names any bit has
    // something to report - the reverse arm that proves a clean run is the check
    // passing, not the check not running.
    bool RelevanceBroken(const std::string& hlslPath)
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const std::string s_file = Sys::CommandLine::Get().Get("-permrel_break");
        return !s_file.empty() && hlslPath.size() >= s_file.size() &&
               hlslPath.compare(hlslPath.size() - s_file.size(), s_file.size(), s_file) == 0;
#else
        (void)hlslPath;
        return false;
#endif
    }

    // The first load of a shader per version of its closure (@p closureTime, MaxIncludeTime): a
    // permutation define the closure names that the mask leaves out means variants that differ
    // only in it share one .ishdr. For a static id the mask is its generated row - the table is
    // stale; for a dynamic id it is what its own files name plus the generated rows of the
    // engine headers they include - a header row is stale, or the parser missed a define or an
    // include. A hot-reload edit is a new version, so it is checked again.
    void CheckRelevance(uint32_t id, const std::string& hlslPath, const std::string& shaderDir,
                        uint64_t closureTime)
    {
        const bool dynamic = DynamicShaderRegistry::IsDynamicId(id);
        if ((!dynamic && id >= static_cast<uint32_t>(ShaderID::Count)) || closureTime == 0) return;
        {
            static std::mutex s_mutex;
            static std::unordered_map<uint32_t, uint64_t> s_checked;
            std::lock_guard<std::mutex> lock(s_mutex);
            auto [it, fresh] = s_checked.try_emplace(id, closureTime);
            if (!fresh && it->second == closureTime) return;
            it->second = closureTime;
        }
        const bool     broken = RelevanceBroken(hlslPath);
        const uint32_t row    = broken ? 0u : (dynamic ? DynamicKeyMask(id) : kRelevantPermMask[id]);
        const uint32_t named  = ClosureBits(hlslPath, shaderDir);
        const uint32_t missing = named & ~row;
        if (dynamic) g_relDynamic.fetch_add(1, std::memory_order_relaxed);
        // Proof of life: a check that never ran would look exactly like a clean one.
        if (g_relChecked.fetch_add(1, std::memory_order_relaxed) == 0)
            LOG_INFO("[PERMREL] first checked '%s' (shader id %u): closure names 0x%08X, row 0x%08X",
                     hlslPath.c_str(), id, named, row);
        if (!missing) return;
        g_relStale.fetch_add(1, std::memory_order_relaxed);
        std::string names;
        for (const PermutationDefine& d : kPermutationDefines)
            if (missing & d.bit) names += (names.empty() ? "" : " ") + std::string(d.name);
        if (dynamic)
            ENGINE_CHECK(false, "[PERMREL] '%s' (dynamic shader %u): its #include closure names %s, which its "
                         "key mask - the defines its own files name plus the engine header rows of "
                         "Graphics/PermutationRelevance.generated.h - leaves out: variants that differ only in "
                         "them share one .ishdr. Run python tools/gen_perm_relevance.py (a header row), or the "
                         "declaration parser missed a define or an #include%s",
                         hlslPath.c_str(), id, names.c_str(), broken ? " (-permrel_break)" : "");
        else
            ENGINE_CHECK(false, "[PERMREL] '%s' (shader id %u): its #include closure names %s, which "
                         "Graphics/PermutationRelevance.generated.h leaves out of the id's mask - variants "
                         "that differ only in them share one .ishdr. Run python tools/gen_perm_relevance.py%s",
                         hlslPath.c_str(), id, names.c_str(), broken ? " (-permrel_break)" : "");
    }

    // ==== The custom-material binding contract ====
    // Surface-material variants checked, and the resources reported
    // (ShaderLibrary::LogMaterialSlotSummary).
    std::atomic<uint32_t> g_slotChecked{ 0 };
    std::atomic<uint32_t> g_slotErrors{ 0 };

    // ==== The declarations held to the DXIL ====
    // Compiled dynamic variants checked against their source declarations, the differences
    // reported, and the sources the parser could not lay out
    // (ShaderLibrary::LogMaterialSlotSummary).
    std::atomic<uint32_t> g_declChecked{ 0 };
    std::atomic<uint32_t> g_declMismatches{ 0 };
    std::atomic<uint32_t> g_declInexact{ 0 };

    // -matdecl_break (harness, MaterialShaderDecl.cpp): named in the report.
    bool DeclBrokenFlag()
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const bool s_broken = Sys::CommandLine::Get().Has("-matdecl_break");
        return s_broken;
#else
        return false;
#endif
    }

    // Once per source and warning: a permutation define tested so the test is a constant
    // (#ifdef ALPHA_TEST).
    void NoteDeclWarnings(const MaterialShaderDecl::Decl& decl)
    {
        static std::mutex            s_mutex;
        static std::set<std::string> s_reported;
        for (const std::string& w : decl.warnings)
        {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                if (!s_reported.insert(w).second) continue;
            }
            LOG_WARNING("[MATDECL] %s", w.c_str());
        }
    }

    // Once per source: its interface stays the DXIL's, and why.
    void NoteInexactDecl(const MaterialShaderDecl::Decl& decl)
    {
        static std::mutex            s_mutex;
        static std::set<std::string> s_reported;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_reported.insert(decl.root).second) return;
        }
        g_declInexact.fetch_add(1, std::memory_order_relaxed);
        LOG_WARNING("[MATDECL] '%s': %zu declaration(s) the parser cannot lay out - its interface stays the "
                    "DXIL reflection's (first: %s)", decl.root.c_str(), decl.inexact.size(),
                    decl.inexact.empty() ? "-" : decl.inexact.front().c_str());
    }

    // -matslot_break (harness): every cbuffer a material declares counts as off
    // its slot, so a material with b8 tunables has something to report - the
    // reverse arm that proves a clean run is the check passing.
    bool MaterialSlotsBroken()
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const bool s_broken = Sys::CommandLine::Get().Has("-matslot_break");
        return s_broken;
#else
        return false;
#endif
    }

    // A surface material (its closure includes custom_surface.hlsli or custom_vertex.hlsli) owns
    // two kinds of slot: the cbuffer at b8 space0 and textures in space3 - the only ones
    // MaterialReflectionSync reflects into the Inspector and the packer binds. A cbuffer or
    // texture the material's own files (outside the engine shader root) declare anywhere else
    // never reaches it (a cbuffer at b0..b7 reads an engine CB, a texture at tN space0 an engine
    // texture or nothing): an ERROR once per (file, resource, register). The engine's own
    // declarations, which the contract headers bring in, are not the material's and are never
    // checked.
    void CheckMaterialSlots(const std::string& hlslPath, const std::string& shaderDir,
                            const ShaderReflect::Reflection& refl)
    {
        const ClosureInfo info = ScanClosure(hlslPath, shaderDir);
        if (!info.surfaceMaterial) return;
        g_slotChecked.fetch_add(1, std::memory_order_relaxed);
        const bool broken = MaterialSlotsBroken();
        for (const ShaderReflect::ResourceBinding& b : refl.bindings)
        {
            if (!info.ownDeclared.count(b.name)) continue;
            const char* kind = nullptr;
            if (b.type == ShaderReflect::ResourceType::CBuffer)
            {
                if (broken || b.bindPoint != 8u || b.space != 0u) kind = "cbuffer";
            }
            else if (b.type == ShaderReflect::ResourceType::Texture)
            {
                if (b.space != 3u) kind = "texture";
            }
            if (!kind) continue;
            char where[48];
            std::snprintf(where, sizeof(where), "%c%u space%u",
                          b.type == ShaderReflect::ResourceType::CBuffer ? 'b' : 't', b.bindPoint, b.space);
            const std::string key = hlslPath + "|" + b.name + "|" + where;
            {
                static std::mutex            s_mutex;
                static std::set<std::string> s_reported;
                std::lock_guard<std::mutex> lock(s_mutex);
                if (!s_reported.insert(key).second) continue;
            }
            g_slotErrors.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("[MATSLOT] '%s': %s '%s' is at %s - a material owns only the cbuffer at b8 space0 and "
                      "textures in space3; this one never reaches the material (Inspector, packer)%s",
                      hlslPath.c_str(), kind, b.name.c_str(), where, broken ? " (-matslot_break)" : "");
        }
    }

} // anonymous namespace

// The permutation a shader is cached under: the key masked to the bits its closure names,
// after one canonicalisation. A static shader that names the blend mode set in the key reads
// ALPHA_BLEND beside it only as the forward switch the mode implies, so both spellings share
// one cached shader (PermutationKey::CanonicalBlend; gen_perm_relevance.py --verify-dxil
// compiles both for every such (shader, mode) pair). Not where nothing is proven: a shader
// that names ALPHA_BLEND but not the mode keeps it (its only forward switch), a dynamic
// shader is user source that may read ALPHA_BLEND for itself, and an id the generator left
// at all bits names nothing for certain. A dynamic shader is masked by DynamicPermMask (the
// same closure rule, from its declarations and the generated header rows) and never
// canonicalised.
uint32_t ShaderLibrary::KeyBits(uint32_t id, PermutationKey perm) noexcept
{
    if (DynamicShaderRegistry::IsDynamicId(id)) return perm.bits & DynamicKeyMask(id);
    const uint32_t mask = RelevantPermMask(id);
    if (id < static_cast<uint32_t>(ShaderID::Count) && mask != ~0u &&
        (perm.bits & PermutationKey::kBlendModes & mask) != 0)
        perm = perm.CanonicalBlend();
    return perm.bits & mask;
}

void ShaderLibrary::LogRelevanceSummary()
{
    LOG_INFO("[PERMREL] summary: checked=%u stale=%u rows=%zu dynamic=%u headers=%zu",
             g_relChecked.load(std::memory_order_relaxed), g_relStale.load(std::memory_order_relaxed),
             std::size(PermutationRelevance::kRows), g_relDynamic.load(std::memory_order_relaxed),
             std::size(PermutationRelevance::kHeaderRows));
}

void ShaderLibrary::LogMaterialSlotSummary()
{
    LOG_INFO("[MATSLOT] summary: checked=%u errors=%u",
             g_slotChecked.load(std::memory_order_relaxed), g_slotErrors.load(std::memory_order_relaxed));
    // The declarations every compiled dynamic variant was held to.
    LOG_INFO("[MATDECL] summary: checked=%u mismatches=%u inexact=%u",
             g_declChecked.load(std::memory_order_relaxed), g_declMismatches.load(std::memory_order_relaxed),
             g_declInexact.load(std::memory_order_relaxed));
}

uint32_t ShaderLibrary::RelevantPermMask(uint32_t id) noexcept
{
    // Out of range = a dynamic (runtime-registered) shader, or PSODesc's kNoPixelShader
    // sentinel; PSODesc::Hash handles the sentinel. A dynamic shader keeps every bit here - the
    // PSO identity, hashed per draw - while KeyBits masks its blob: a finer PSO key than the
    // blob's is at worst a duplicate PSO, never two blobs under one PSO.
    if (id >= static_cast<uint32_t>(ShaderID::Count)) return ~0u;
    return kRelevantPermMask[id];
}

uint32_t ShaderLibrary::DynamicPermMask(const MaterialShaderDecl::Decl& decl)
{
    if (!decl.read || decl.permBits == ~0u || !decl.unresolvedIncludes.empty()) return ~0u;
    uint32_t mask = decl.permBits;
    for (const std::string& inc : decl.engineIncludes)
    {
        const PermutationRelevance::HeaderRow* row = nullptr;
        for (const PermutationRelevance::HeaderRow& r : PermutationRelevance::kHeaderRows)
            if (_stricmp(r.path, inc.c_str()) == 0) { row = &r; break; }
        // No row: the header's closure names no bit (the generator lists every one
        // that names a bit, verify's perm_relevance gate keeps the table current and
        // CheckRelevance reports a bit it lacks).
        if (row) mask |= row->mask;
    }
    return mask;
}

void ShaderLibrary::Init(IGraphicsDevice& gfx, const char* shaderDir)
{
    m_gfx       = &gfx;
    m_shaderDir = shaderDir ? shaderDir : "";
    if (!m_shaderDir.empty() && m_shaderDir.back() != '/' && m_shaderDir.back() != '\\')
        m_shaderDir += '/';
}

std::atomic<std::uint32_t> ShaderLibrary::s_failedShaderTotal{ 0 };

// Decrements this instance's share of the process-wide tally. See the note
// on TotalFailedShaders() in the header.
ShaderLibrary::~ShaderLibrary()
{
    ShaderRecipes::ForgetOwner(this);
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_failedShaders.empty())
        s_failedShaderTotal.fetch_sub(static_cast<std::uint32_t>(m_failedShaders.size()),
                                      std::memory_order_relaxed);
}

void ShaderLibrary::Shutdown(IGraphicsDevice& /*gfx*/)
{
    // Nothing this library held is in use any more; a hot reload must not
    // wait for it to let go (ShaderRecipes::CheckCoverage).
    ShaderRecipes::ForgetOwner(this);
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_gfx)
        for (auto& kv : m_shaderCache)
            m_gfx->DestroyShader(kv.second);
    m_shaderCache.clear();
    m_reflectionCache.clear();
    m_reflectionUnions.clear();
    m_interfaces.clear();
    m_loadStamps.clear();
    // Dynamic entries are DynamicShaderRegistry's and outlive this library: a rebuilt pass
    // resolves the ids it was handed before.
    m_gfx = nullptr;
}

void ShaderLibrary::Register(ShaderID id, RHI::ShaderStage stage,
                              const char* hlslFile, const char* entry)
{
    const uint32_t idx = static_cast<uint32_t>(id);
    // ENGINE_CHECK, not assert: Release - the build that runs - would index past m_registry.
    if (!ENGINE_CHECK(idx < static_cast<uint32_t>(ShaderID::Count),
                      "ShaderLibrary::Register: id %u is not a static ShaderID ('%s')", idx,
                      hlslFile ? hlslFile : ""))
        return;
    m_registry[idx].hlslFile   = hlslFile ? hlslFile : "";
    m_registry[idx].entry      = entry    ? entry    : "main";
    m_registry[idx].stage      = stage;
    m_registry[idx].registered = true;
}

const RHI::Shader* ShaderLibrary::GetShader(ShaderID id, PermutationKey perm)
{
    return GetShaderInternal(static_cast<uint32_t>(id), perm);
}

const RHI::Shader* ShaderLibrary::GetDynamicShader(uint32_t dynId, PermutationKey perm)
{
    if (dynId == kInvalidDynShaderID) return nullptr;
    return GetShaderInternal(dynId, perm);
}

const ShaderReflect::Reflection* ShaderLibrary::GetReflection(ShaderID id, PermutationKey perm)
{
    return GetReflectionInternal(static_cast<uint32_t>(id), perm);
}

const ShaderReflect::Reflection* ShaderLibrary::GetDynamicReflection(uint32_t dynId, PermutationKey perm)
{
    if (dynId == kInvalidDynShaderID) return nullptr;
    return GetReflectionInternal(dynId, perm);
}

const RHI::Shader* ShaderLibrary::GetShaderInternal(uint32_t id, PermutationKey perm)
{
    // Two specific blend modes at once is no blend mode (PermutationKey.h):
    // the PSO and the shader would each pick a different one.
    ENGINE_CHECK_ONCE(perm.LegalBlend(), "ShaderLibrary: shader id %u requested with permutation 0x%08X - "
                      "more than one of ADDITIVE / PREMULTIPLIED / MULTIPLY / TRANSMISSIVE_BLEND", id, perm.bits);
    // Strip the permutation bits this shader's source (and its whole #include closure) never
    // tests, after canonicalising the blend bits. Done once, here, so everything downstream - the
    // in-memory cache key, the negative cache, LoadOrCompile's CachePath and MakeDefines - sees
    // the same collapsed key and shares one .ishdr blob.
    perm.bits = KeyBits(id, perm);

    const CacheKey key{ id, perm.bits };

    std::shared_ptr<InFlight> flight;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        for (;;)
        {
            auto it = m_shaderCache.find(key);
            if (it != m_shaderCache.end())
                return &it->second;
            // Negative cache: a previous attempt for this (id, perm) failed and
            // there's no point re-running DXC every frame against a broken file.
            // Hot-reload's ClearCaches drops this map so future edits retry.
            if (m_failedShaders.find(key) != m_failedShaders.end())
                return nullptr;
            // Another thread is building this very (id, perm): wait for it and take its result instead
            // of building a twin.
            const auto f = m_inFlight.find(key);
            if (f == m_inFlight.end()) break;
            const std::shared_ptr<InFlight> other = f->second;
            PROF_SCOPE("Shader.WaitInFlight");
            m_inFlightCv.wait(lk, [&other] { return other->done; });
        }
        flight = std::make_shared<InFlight>();
        m_inFlight.emplace(key, flight);
    }
    // Every exit below hands the key back - the waiters re-check the caches.
    struct FlightRelease
    {
        ShaderLibrary& lib; CacheKey key; std::shared_ptr<InFlight> f;
        ~FlightRelease()
        {
            {
                std::lock_guard<std::mutex> lk(lib.m_mutex);
                f->done = true;
                lib.m_inFlight.erase(key);
            }
            lib.m_inFlightCv.notify_all();
        }
    } flightRelease{ *this, key, flight };

    // A miss: this library has not loaded (id, perm) yet. Each costs the .ishdr load (or a DXC
    // compile), the reflection, the //@ annotation parse and the binding checks below (~4 ms).
    // Counted on every thread (Graphics/PipelineStats.h): record workers miss here too.
    PROF_SCOPE("Shader.Miss");
    PipelineStats::shaderMisses.fetch_add(1, std::memory_order_relaxed);
    RHI::Shader shader;
    std::vector<uint8_t> bytecode;
    LoadStamp stamp;
    bool loaded = false;
    {
        PROF_SCOPE("Shader.LoadOrCompile");
        loaded = LoadOrCompile(id, perm, shader, bytecode, stamp);
    }
    if (!loaded)
        return nullptr;

    // Reflect off the fresh bytecode. A failure is not fatal: the shader still runs, only
    // auto-UI / binding discovery is skipped for it.
    ShaderReflect::Reflection reflection;
    bool reflected = false;
    {
        PROF_SCOPE("Shader.Reflect");
        reflected = ShaderReflect::Reflect(bytecode.data(), bytecode.size(), reflection);
    }

    // Stamp the authoring annotations (//@default / @min / @max / @label) from the HLSL source
    // onto the reflected cbuffer vars, so the material layer can seed per-param defaults and
    // inspector ranges (DXC reflection carries no cbuffer defaults). The path resolves as in
    // LoadOrCompile; CachedSource reads it through AssetFS.
    if (reflected)
    {
        PROF_SCOPE("Shader.Annotate");
        if (const ShaderEntry* entry = GetRegistryEntry(id))
        {
            const std::string& f = entry->hlslFile;
            const bool isAbsolute = (f.size() >= 2 && f[1] == ':') ||
                                    (f.size() >= 2 && f[0] == '\\' && f[1] == '\\') ||
                                    (!f.empty() && f[0] == '/');
            const bool hasDir = f.find('/') != std::string::npos || f.find('\\') != std::string::npos;
            const std::string hlslPath = (isAbsolute || hasDir) ? f : (m_shaderDir + f);
            // The source read once per write time (CachedSource), stamped by the one copy of the
            // grammar (MaterialShaderDecl).
            if (const auto src = CachedSource(hlslPath))
                MaterialShaderDecl::StampSourceAnnotations(*src, reflection, f);
            // A surface material's own resources sit in its two slots.
            if (id >= static_cast<uint32_t>(ShaderID::Count))
            {
                CheckMaterialSlots(hlslPath, m_shaderDir, reflection);
                // The declarations its interface is read from, held to this variant's DXIL.
                CheckDeclaration(id, perm, reflection);
            }
        }
    }

    // Binding contract check, a walk over the reflection already paid for: does every register
    // the shader declares exist in the root signature that will be bound? A mismatch is
    // otherwise invisible - the draw succeeds and samples whatever descriptor occupies the slot.
    // Reported, never fatal, and deduplicated process-wide: ShaderLibrary is not a singleton
    // (~60 instances, one per pass / subsystem), so a per-object set would re-log the same
    // finding once per owning pass.
    if (reflected)
    {
        PROF_SCOPE("Shader.Validate");
        if (const ShaderEntry* entry = GetRegistryEntry(id))
        {
            static std::mutex               s_reportedMutex;
            static std::set<std::string>    s_reported;

            std::vector<std::string> problems;
            if (!RHI::Validation::ValidateShaderBindings(entry->hlslFile, entry->stage,
                                                         reflection, problems))
            {
                std::lock_guard<std::mutex> rl(s_reportedMutex);
                for (const std::string& p : problems)
                    if (s_reported.insert(p).second)
                        LOG_WARNING("Shader binding mismatch: %s", p.c_str());
            }

            // Layout contract check: every registered C++ struct this shader mirrors (LightCB,
            // PerViewCB, MaterialGPUData, ...) member by member against the same reflection. Reports
            // itself (an ERROR per mismatch, once per process).
            Layout::CheckRegistered(entry->hlslFile.c_str(), reflection);
        }
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    auto [it, inserted] = m_shaderCache.emplace(key, shader);
    if (!inserted)
    {
        // Still possible across a ClearCaches (the key went and came back while this build ran):
        // the cached handle stands, and this one goes back to the pool instead of leaking a slot.
        if (m_gfx) m_gfx->DestroyShader(shader);
        return &it->second;
    }
    if (reflected) m_reflectionCache.emplace(key, std::move(reflection));
    m_loadStamps[key] = std::move(stamp);
    return &it->second;
}

const ShaderReflect::Reflection* ShaderLibrary::GetReflectionInternal(uint32_t id, PermutationKey perm)
{
    // Warm the shader (and reflection) cache if needed.
    if (!GetShaderInternal(id, perm)) return nullptr;

    // Same collapse GetShaderInternal applied, or the lookup misses the entry
    // it just warmed.
    const CacheKey key{ id, KeyBits(id, perm) };
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_reflectionCache.find(key);
    return it != m_reflectionCache.end() ? &it->second : nullptr;
}

// The reflection is looked up again under the lock rather than read through
// GetReflectionInternal's pointer, which a ClearCaches on another thread may have dropped
// in between.
bool ShaderLibrary::Report(uint32_t id, PermutationKey perm, ShaderReport& out)
{
    out = ShaderReport{};
    const bool built = GetReflectionInternal(id, perm) != nullptr;
    const CacheKey key{ id, KeyBits(id, perm) };
    out.permBits = key.permBits;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (const ShaderEntry* e = GetRegistryEntry(id))
    {
        out.file  = e->hlslFile;
        out.entry = e->entry;
    }
    const auto r = m_reflectionCache.find(key);
    if (!built || r == m_reflectionCache.end()) return false;
    if (const auto s = m_loadStamps.find(key); s != m_loadStamps.end()) out.cachePath = s->second.cachePath;
    out.instructions = r->second.instructionCount;
    return true;
}

namespace
{
    // Adds to @p into what @p from declares and @p into lacks, by name: bindings, cbuffers and
    // their members (a member either variant reads is used), struct elements; the requires-flags
    // and engineRolesExplicit are ORed. A resource keeps the register its declaration gives it in
    // every variant that keeps it.
    void MergeReflection(ShaderReflect::Reflection& into, const ShaderReflect::Reflection& from)
    {
        for (const ShaderReflect::ResourceBinding& b : from.bindings)
            if (std::none_of(into.bindings.begin(), into.bindings.end(), [&](const auto& x) {
                    return x.name == b.name && x.type == b.type; }))
                into.bindings.push_back(b);
        for (const ShaderReflect::CBufferLayout& cb : from.cbuffers)
        {
            auto it = std::find_if(into.cbuffers.begin(), into.cbuffers.end(),
                                   [&](const auto& x) { return x.name == cb.name; });
            if (it == into.cbuffers.end()) { into.cbuffers.push_back(cb); continue; }
            it->sizeBytes = std::max(it->sizeBytes, cb.sizeBytes);
            for (const ShaderReflect::CBufferVar& v : cb.vars)
            {
                auto vit = std::find_if(it->vars.begin(), it->vars.end(),
                                        [&](const auto& x) { return x.name == v.name; });
                if (vit == it->vars.end()) it->vars.push_back(v);
                else                       vit->used = vit->used || v.used;
            }
        }
        for (const ShaderReflect::StructElement& e : from.elements)
            if (std::none_of(into.elements.begin(), into.elements.end(),
                             [&](const auto& x) { return x.resource == e.resource; }))
                into.elements.push_back(e);
        into.requiresFlags |= from.requiresFlags;
        into.engineRolesExplicit = into.engineRolesExplicit || from.engineRolesExplicit;
        into.readsFrameTime = into.readsFrameTime || from.readsFrameTime;
    }
}

const ShaderReflect::Reflection* ShaderLibrary::GetDynamicReflectionUnion(
    uint32_t dynId, std::initializer_list<PermutationKey> perms)
{
    if (dynId == kInvalidDynShaderID) return nullptr;
    // The keys the variants are really cached under: two that collapse are one.
    std::vector<uint32_t> keys;
    for (PermutationKey p : perms) keys.push_back(KeyBits(dynId, p));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    // Warm every variant first: a compile runs outside the library lock.
    std::vector<const ShaderReflect::Reflection*> parts;
    for (uint32_t k : keys)
        if (const ShaderReflect::Reflection* r = GetReflectionInternal(dynId, PermutationKey{ k }))
            parts.push_back(r);
    if (parts.size() <= 1) return parts.empty() ? nullptr : parts[0];

    std::lock_guard<std::mutex> lk(m_mutex);
    auto [it, fresh] = m_reflectionUnions.try_emplace({ dynId, keys });
    if (fresh)
    {
        it->second = *parts[0];
        for (size_t i = 1; i < parts.size(); ++i) MergeReflection(it->second, *parts[i]);
    }
    return &it->second;
}

// The parse is fetched outside the lock (it reads files); two threads racing here store one
// of two identical parses.
std::shared_ptr<const MaterialShaderDecl::Decl> ShaderLibrary::GetDynamicDecl(uint32_t dynId)
{
    if (!DynamicShaderRegistry::IsDynamicId(dynId)) return nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (const auto it = m_interfaces.find(dynId); it != m_interfaces.end()) return it->second;
    }
    const ShaderEntry* e = GetRegistryEntry(dynId);
    if (!e) return nullptr;
    std::shared_ptr<const MaterialShaderDecl::Decl> decl = MaterialShaderDecl::Get(e->hlslFile, m_shaderDir);
    if (decl->read && !decl->Exact()) NoteInexactDecl(*decl);
    if (!decl->warnings.empty()) NoteDeclWarnings(*decl);
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_interfaces.try_emplace(dynId, std::move(decl)).first->second;
}

const ShaderReflect::Reflection* ShaderLibrary::GetDynamicInterface(uint32_t dynId)
{
    // The Decl lives on in m_interfaces: the pointer outlives this local.
    const std::shared_ptr<const MaterialShaderDecl::Decl> decl = GetDynamicDecl(dynId);
    return (decl && decl->Exact()) ? &decl->iface : nullptr;
}

ShaderLibrary::DynamicReads ShaderLibrary::GetDynamicReads(uint32_t dynId) const
{
    DynamicReads r;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& [key, refl] : m_reflectionCache)
    {
        if (key.shaderID != dynId) continue;
        r.known = true;
        for (const ShaderReflect::CBufferLayout& cb : refl.cbuffers)
            for (const ShaderReflect::CBufferVar& v : cb.vars)
                if (v.used) r.vars.insert(v.name);
        for (const ShaderReflect::ResourceBinding& b : refl.bindings) r.bindings.insert(b.name);
    }
    return r;
}

// Every compiled variant of a dynamic shader whose declarations parse exactly:
// the declarations its interface is read from must be what the DXIL binds and
// lays out (MaterialShaderDecl::CrossCheck). Each difference is an ERROR once
// per (source, difference) - a parser bug, or a source the parser misreads -
// counted in the [MATDECL] summary.
void ShaderLibrary::CheckDeclaration(uint32_t id, PermutationKey perm, const ShaderReflect::Reflection& refl)
{
    const std::shared_ptr<const MaterialShaderDecl::Decl> decl = GetDynamicDecl(id);
    if (!decl || !decl->Exact()) return;
    g_declChecked.fetch_add(1, std::memory_order_relaxed);
    for (const std::string& m : MaterialShaderDecl::CrossCheck(*decl, refl))
    {
        {
            static std::mutex            s_mutex;
            static std::set<std::string> s_reported;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_reported.insert(decl->root + "|" + m).second) continue;
        }
        g_declMismatches.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("[MATDECL] '%s' (perm 0x%08X): %s - the declarations its interface is read from disagree "
                  "with this variant's DXIL%s", decl->root.c_str(), perm.bits, m.c_str(),
                  DeclBrokenFlag() ? " (-matdecl_break)" : "");
    }
}

const ShaderLibrary::ShaderEntry* ShaderLibrary::GetRegistryEntry(uint32_t id) const
{
    if (id < static_cast<uint32_t>(ShaderID::Count))
    {
        const ShaderEntry& e = m_registry[id];
        return e.registered ? &e : nullptr;
    }
    // Every library resolves a dynamic id through the one registry: no per-library copy,
    // nothing to keep in step.
    return DynamicShaderRegistry::Get().Lookup(id);
}

uint32_t ShaderLibrary::RegisterDynamic(const char* hlslFile, RHI::ShaderStage stage,
                                        const char* entry)
{
    if (!hlslFile || !*hlslFile)
    {
        LOG_WARNING("ShaderLibrary::RegisterDynamic: empty path");
        return kInvalidDynShaderID;
    }
    return DynamicShaderRegistry::Get().Register(hlslFile, stage, entry);
}

uint32_t ShaderLibrary::FindDynamic(const char* hlslFile, RHI::ShaderStage stage,
                                    const char* entry) const
{
    return DynamicShaderRegistry::Get().Find(hlslFile, stage, entry);
}

ShaderLibrary::CacheCompile ShaderLibrary::CompileForCache(const std::string& hlslFile, const std::string& source,
                                                         RHI::ShaderStage stage, PermutationKey perm,
                                                         const char* entry) const
{
    CacheCompile out;
    const auto t0 = std::chrono::steady_clock::now();
    // The path and options exactly as LoadOrCompile resolves and builds them.
    const bool isAbsolute =
        (hlslFile.size() >= 2 && hlslFile[1] == ':') ||
        (hlslFile.size() >= 2 && hlslFile[0] == '\\' && hlslFile[1] == '\\') ||
        (!hlslFile.empty() && hlslFile[0] == '/');
    const bool hasDirComponent = hlslFile.find('/') != std::string::npos || hlslFile.find('\\') != std::string::npos;
    const std::string hlslPath = (isAbsolute || hasDirComponent) ? hlslFile : (m_shaderDir + hlslFile);
    out.cachePath = CachePath(hlslPath, perm, entry, stage);

    DxcCompiler::CompileOptions opts;
    opts.sourceName = hlslPath;
    opts.entry      = entry;
    opts.stage      = stage;
    opts.includeDir = m_shaderDir;
#ifdef _DEBUG
    opts.debug      = true;
#else
    opts.debug      = false;
#endif
    for (auto& [name, val] : MakeDefines(perm))
        opts.defines.push_back({ name, val });
    if (stage == RHI::ShaderStage::VS)      opts.defines.push_back({ "MATERIAL_STAGE_VS", "1" });
    else if (stage == RHI::ShaderStage::PS) opts.defines.push_back({ "MATERIAL_STAGE_PS", "1" });

    const DxcCompiler::CompileResult cr = DxcCompiler::Compile(source.data(), source.size(), opts);
    out.error = cr.errorMsg;
    if (cr.ok)
    {
        out.container = ShaderDiskCache::BuildContainer(entry, stage, cr.dxil.data(), cr.dxil.size());
        out.ok = true;
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

bool ShaderLibrary::PreloadShader(ShaderID id, PermutationKey perm)
{
    return GetShader(id, perm) != nullptr;
}

namespace
{
    thread_local int t_cacheGateShared    = 0;
    thread_local int t_cacheGateExclusive = 0;

    std::shared_mutex& CacheGateMutex()
    {
        static std::shared_mutex s_mutex;
        return s_mutex;
    }
}

ShaderCacheGate::Shared::Shared() : m_locked(t_cacheGateShared == 0 && t_cacheGateExclusive == 0)
{
    if (m_locked) CacheGateMutex().lock_shared();
    ++t_cacheGateShared;
}

ShaderCacheGate::Shared::~Shared()
{
    --t_cacheGateShared;
    if (m_locked) CacheGateMutex().unlock_shared();
}

ShaderCacheGate::Exclusive::Exclusive()
{
    // A warm-up clearing a cache would wait for itself: said, and not locked.
    (void)ENGINE_CHECK_ONCE(t_cacheGateShared == 0,
        "ShaderCacheGate: a cache clear inside a warm-up's shared scope - it would wait for itself");
    m_locked = t_cacheGateExclusive == 0 && t_cacheGateShared == 0;
    if (m_locked) CacheGateMutex().lock();
    ++t_cacheGateExclusive;
}

ShaderCacheGate::Exclusive::~Exclusive()
{
    --t_cacheGateExclusive;
    if (m_locked) CacheGateMutex().unlock();
}

bool ShaderLibrary::ClearCaches()
{
    // A worker warm-up using this library's entries finishes its material first.
    ShaderCacheGate::Exclusive gate;
    std::lock_guard<std::mutex> lk(m_mutex);
    // Keep what a reload would load byte-for-byte again (see the header): the .ishdr is the file
    // it was loaded from, untouched since, and its source closure is not newer than it -
    // LoadOrCompile's own staleness rule. The shader pool never recycles a slot, so re-creating
    // the rest of the library for an edit to one of its shaders would only burn slots.
    std::unordered_map<std::string, uint64_t> sourceTimes;   // many perms, one source
    auto unchanged = [&](const LoadStamp& s)
    {
        if (s.cacheTime == 0 || ShaderDiskCache::FileWriteTime(s.cachePath) != s.cacheTime)
            return false;
        auto [st, fresh] = sourceTimes.try_emplace(s.hlslPath, 0);
        if (fresh) st->second = ShaderDiskCache::MaxIncludeTime(s.hlslPath, m_shaderDir);
        return !(st->second != 0 && st->second > s.cacheTime);
    };
    // Release the DXIL behind every dropped handle: the backend shader pool is append-only, so
    // otherwise every old blob would stay alive for the process lifetime. PSOs already built
    // from them are unaffected (they hold compiled ISA); a pass that kept a stale RHI::Shader*
    // across the reload gets a one-time ENGINE_CHECK from CreatePipelineState instead of
    // silently rebuilding the old shader.
    bool changed = false;
    for (auto it = m_shaderCache.begin(); it != m_shaderCache.end();)
    {
        auto stamp = m_loadStamps.find(it->first);
        if (stamp != m_loadStamps.end() && unchanged(stamp->second))
        {
            ++it;
            continue;
        }
        if (m_gfx) m_gfx->DestroyShader(it->second);
        m_reflectionCache.erase(it->first);
        if (stamp != m_loadStamps.end()) m_loadStamps.erase(stamp);
        it = m_shaderCache.erase(it);
        changed = true;
    }
    // fetch_sub of the whole size, taken BEFORE the clear: this wipes the
    // entire map, it does not erase one entry.
    if (!m_failedShaders.empty())
    {
        s_failedShaderTotal.fetch_sub(static_cast<std::uint32_t>(m_failedShaders.size()),
                                      std::memory_order_relaxed);
        changed = true;
    }
    m_failedShaders.clear();   // hot-reload retries previously-broken shaders
    // A declaration whose files were written since its parse goes (and is parsed again on next
    // use), and the epoch moves so every synced material reads it again - also when this
    // library never compiled a variant of it (the interface needs no compile), so nothing above
    // was dropped for it.
    for (auto it = m_interfaces.begin(); it != m_interfaces.end();)
    {
        if (it->second && MaterialShaderDecl::Stale(*it->second)) { it = m_interfaces.erase(it); changed = true; }
        else ++it;
    }
    // Every Reflection* of a dropped shader is now dangling and every completed
    // MaterialReflectionSync is stale (see GetReloadEpoch()). Nothing dropped: every reflection
    // is still what it was, so the epoch stays. A merged reflection (GetDynamicReflectionUnion)
    // may hold a dropped variant's bindings: all of them go, and rebuild on next use.
    if (changed)
    {
        m_reloadEpoch.fetch_add(1, std::memory_order_relaxed);
        m_reflectionUnions.clear();
    }
    // An edited dynamic source may name other bits: every cached key mask is computed again on
    // its next use (from the parse, cheap while unchanged).
    DynamicShaderRegistry::Get().ForgetKeyMasks();
    // Everything an edit reached was dropped above (its .ishdr was rewritten)
    // and reloads on next use: the hot-reload receipt counts this library as
    // reloaded (ShaderRecipes::CheckCoverage).
    ShaderRecipes::NoteOwnerCleared(this);
    return changed;
}

std::string ShaderLibrary::GetCompileError(ShaderID id, PermutationKey perm)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const uint32_t rawId = static_cast<uint32_t>(id);
    auto it = m_failedShaders.find({ rawId, KeyBits(rawId, perm) });
    return it != m_failedShaders.end() ? it->second : std::string{};
}

std::string ShaderLibrary::GetDynamicCompileError(uint32_t dynId, PermutationKey perm)
{
    if (dynId == kInvalidDynShaderID) return {};
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_failedShaders.find({ dynId, KeyBits(dynId, perm) });
    return it != m_failedShaders.end() ? it->second : std::string{};
}

ShaderLibrary::DynamicShaderStatus ShaderLibrary::GetDynamicShaderStatus(uint32_t dynId) const
{
    DynamicShaderStatus s;
    if (dynId == kInvalidDynShaderID) return s;
    // A scan, not a lookup: the caches are keyed by (id, permutation) and the
    // question is about every permutation. Diagnostics only - a few hundred
    // entries at most.
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& [key, shader] : m_shaderCache)
        if (key.shaderID == dynId) ++s.compiled;
    for (const auto& [key, error] : m_failedShaders)
    {
        if (key.shaderID != dynId) continue;
        if (s.failed++ == 0)
        {
            s.failedPermBits = key.permBits;
            s.error          = error;
        }
    }
    return s;
}

bool ShaderLibrary::LoadOrCompile(uint32_t id, PermutationKey perm,
                                  RHI::Shader& outShader,
                                  std::vector<uint8_t>& outBytecode,
                                  LoadStamp& outStamp)
{
    outBytecode.clear();
    const ShaderEntry* entryPtr = GetRegistryEntry(id);
    if (!entryPtr)
    {
        LOG_ERROR("ShaderLibrary: shader id %u not registered", id);
        return false;
    }
    if (!m_gfx)
    {
        LOG_ERROR("ShaderLibrary: not initialised (call Init first)");
        return false;
    }

    const ShaderEntry& entry     = *entryPtr;
    // Resolve the path: Register() passes bare file names ("GBuffer.vs.hlsl") that need
    // shaderDir prepended; the Resource Panel's drag-drop passes project-root-relative paths
    // ("asset/Shaders/my_custom.ps.hlsl") that must not be prefixed. A path that holds a
    // directory separator, or an absolute one (drive letter, UNC, rooted slash), is left alone.
    const bool isAbsolute =
        (entry.hlslFile.size() >= 2 && entry.hlslFile[1] == ':') ||
        (entry.hlslFile.size() >= 2 && entry.hlslFile[0] == '\\' && entry.hlslFile[1] == '\\') ||
        (!entry.hlslFile.empty() && entry.hlslFile[0] == '/');
    const bool hasDirComponent =
        entry.hlslFile.find('/')  != std::string::npos ||
        entry.hlslFile.find('\\') != std::string::npos;
    const std::string  hlslPath = (isAbsolute || hasDirComponent)
        ? entry.hlslFile
        : (m_shaderDir + entry.hlslFile);
    const std::string  cachePath = CachePath(hlslPath, perm, entry.entry, entry.stage);

    DxcCompiler::CompileOptions opts;
    opts.sourceName = hlslPath;
    opts.entry      = entry.entry;
    opts.stage      = entry.stage;
    opts.includeDir = m_shaderDir;
#ifdef _DEBUG
    opts.debug      = true;
#else
    opts.debug      = false;
#endif
    for (auto& [name, val] : MakeDefines(perm))
        opts.defines.push_back({ name, val });

    // Stage selector for unified single-file materials (custom_material.hlsli): the same source
    // compiles once per stage, and the stage define picks which entry point / hook is live; DXC
    // dead-strips the other (a pixel hook's Sample() / ddx() vanish when compiled as VS).
    // Shaders that do not reference these macros are unaffected.
    if (entry.stage == RHI::ShaderStage::VS)
        opts.defines.push_back({ "MATERIAL_STAGE_VS", "1" });
    else if (entry.stage == RHI::ShaderStage::PS)
        opts.defines.push_back({ "MATERIAL_STAGE_PS", "1" });

    // Every outcome is recorded - a failure too - so a hot reload can compile
    // this recipe's new source before it clears anything (ShaderRecipes.h).
    auto note = [&](bool ok)
    {
        ShaderRecipes::Load l;
        l.opts      = opts;
        l.cachePath = cachePath;
        l.scanDir   = m_shaderDir;
        l.owner     = this;
        l.tag       = "ShaderLibrary";
        l.ok        = ok;
        ShaderRecipes::Note(l);
    };

    // Disk cache first: skip it when the HLSL source or any transitive `#include` is newer than
    // the cached .ishdr. Checking only the root .hlsl would miss an edit to a shared header like
    // material.hlsli and load a stale binary compiled against the old struct layout.
    const uint64_t hlslTime  = ShaderDiskCache::MaxIncludeTime(hlslPath, m_shaderDir);
    const uint64_t cacheTime = ShaderDiskCache::FileWriteTime(cachePath);
    const bool     cacheStale = (hlslTime != 0 && hlslTime > cacheTime);
    // This source version's closure against the generated relevance row.
    CheckRelevance(id, hlslPath, m_shaderDir, hlslTime);

    if (cacheStale)
        LOG_INFO("ShaderLibrary: source (or #include) newer than cache, recompiling '%s' (perm=0x%08X)",
                 entry.hlslFile.c_str(), perm.bits);

    std::vector<uint8_t> sc;
    if (!cacheStale)
        sc = ReadFile(cachePath);

    if (!sc.empty() &&
        Resource::CheckHeader(sc.data(), sc.size(), Resource::MAGIC_SHADER).status == Resource::HeaderStatus::Ok)
    {
        const Resource::ShaderMetadata* meta =
            Resource::GetMetadata<Resource::ShaderMetadata>(sc.data());
        const uint8_t* dxbc     = Resource::GetPayload(sc.data());
        const size_t   dxbcSize = Resource::GetHeader(sc.data())->dataSize;
        const auto     stage    = static_cast<RHI::ShaderStage>(meta->stage);

        if (m_gfx->CreateShader(stage, dxbc, dxbcSize, outShader))
        {
            outBytecode.assign(dxbc, dxbc + dxbcSize);
            outStamp = LoadStamp{ hlslPath, cachePath, cacheTime };
            LOG_INFO("ShaderLibrary: loaded cached '%s' (perm=0x%08X)",
                     entry.hlslFile.c_str(), perm.bits);
            note(true);
            return true;
        }
        LOG_WARNING("ShaderLibrary: cached shader invalid, recompiling '%s'",
                    entry.hlslFile.c_str());
    }

    // Compile from HLSL via DXC.
    std::vector<uint8_t> src = ReadFile(hlslPath);
    if (src.empty())
    {
        LOG_ERROR("ShaderLibrary: cannot read '%s'", hlslPath.c_str());
        note(false);
        return false;
    }

    const DxcCompiler::CompileResult cr = DxcCompiler::Compile(src.data(), src.size(), opts);
    if (!cr.ok)
    {
        note(false);
        LOG_ERROR("ShaderLibrary: DXC compile failed for '%s' (perm=0x%08X)\n%s",
                  entry.hlslFile.c_str(), perm.bits, cr.errorMsg.c_str());
        // Negative-cache the failure so per-frame draw loops do not re-run DXC and spam the log.
        // ClearCaches (hot reload on a file change) drops the entry: the natural retry trigger.
        std::lock_guard<std::mutex> lk(m_mutex);

        // Count only a genuine first insert. operator[] below overwrites, and the miss check that
        // led here (GetShaderInternal) ran in a separate critical section, so two threads can both
        // miss, both run DXC, both fail and both arrive here with the same key. A naive increment
        // would double-count and never return to zero.
        const CacheKey failKey{ id, perm.bits };
        if (m_failedShaders.find(failKey) == m_failedShaders.end())
            s_failedShaderTotal.fetch_add(1, std::memory_order_relaxed);

        m_failedShaders[failKey] =
            cr.errorMsg.empty() ? std::string{ "DXC compile failed (no diagnostics)" }
                                : cr.errorMsg;
        return false;
    }
    if (!cr.errorMsg.empty())
    {
        // Warnings: keep going.
        LOG_WARNING("ShaderLibrary: DXC warnings for '%s' (perm=0x%08X)\n%s",
                    entry.hlslFile.c_str(), perm.bits, cr.errorMsg.c_str());
    }

    const void*  dxil     = cr.dxil.data();
    const size_t dxilSize = cr.dxil.size();

    if (!m_gfx->CreateShader(entry.stage, dxil, dxilSize, outShader))
    {
        LOG_ERROR("ShaderLibrary: CreateShader failed for '%s'", entry.hlslFile.c_str());
        return false;
    }
    note(true);

    outBytecode = cr.dxil;

    // Write the cache. The compile flags (Debug / Release aliasing, the dangerous half) and the
    // source path are in the cache file name (CachePath); a DXC upgrade alone leaves old blobs
    // in use.
    // TODO: store the DXC version and a hash of the sorted define list in the .ishdr header and reject a mismatch at load.
    // Neither fits yet: Resource::ShaderMetadata (AssetHeader.h) has only 2 padding bytes free,
    // so it needs a format-version bump, and dxcapi.h has no version macro (IDxcVersionInfo must
    // be queried off the compiler in DxcCompiler.cpp).
    {
        const std::vector<uint8_t> blob =
            ShaderDiskCache::BuildContainer(entry.entry, entry.stage, dxil, dxilSize);
        if (WriteFile(cachePath, blob))
            LOG_INFO("ShaderLibrary: compiled + cached '%s' (perm=0x%08X, %zu bytes)",
                     entry.hlslFile.c_str(), perm.bits, blob.size());
        else
            LOG_WARNING("ShaderLibrary: compiled '%s' but failed to write cache",
                        entry.hlslFile.c_str());
    }
    // After the write: a cache that could not be written leaves an older (or
    // no) file behind, which ClearCaches then reads as "reload this one".
    outStamp = LoadStamp{ hlslPath, cachePath, ShaderDiskCache::FileWriteTime(cachePath) };

    return true;
}

// Thin forward: the naming scheme lives in ShaderDiskCache::CachePath, which the passes that
// compile their own shaders use too. Do not fork it: two schemes that disagree load each
// other's bytecode.
std::string ShaderLibrary::CachePath(const std::string& hlslPath,
                                      PermutationKey perm,
                                      const std::string& entryPoint,
                                      RHI::ShaderStage stage) const
{
    // Must match the flag LoadOrCompile compiles with.
#ifdef _DEBUG
    constexpr bool kDebugFlags = true;
#else
    constexpr bool kDebugFlags = false;
#endif
    return ShaderDiskCache::CachePath(m_cacheDir, m_shaderDir, hlslPath,
                                      perm.bits, entryPoint, stage, kDebugFlags);
}

std::vector<uint8_t> ShaderLibrary::ReadFile(const std::string& path)
{
    // Through AssetFS: the pak when mounted (game build), else the disk (editor / dev).
    std::vector<uint8_t> buf;
    ::Resource::AssetFS::Get().ReadFile(path, buf);
    return buf;
}

bool ShaderLibrary::WriteFile(const std::string& path, const std::vector<uint8_t>& data)
{
    // Temp file + rename (creates the directory too): a render worker loading
    // the same .ishdr, or another engine sharing shader_cache_dxil/, never
    // reads a half-written container.
    return ShaderDiskCache::WriteFileAtomic(path, data);
}

std::vector<std::pair<std::string,std::string>>
ShaderLibrary::MakeDefines(PermutationKey perm)
{
    // Generated from PERMUTATION_KEY_BITS (PermutationKey.h): one NAME=0|1 define per bit, so a
    // new bit cannot exist without reaching DXC.
    std::vector<std::pair<std::string,std::string>> defs;
    defs.reserve(std::size(kPermutationDefines));
    for (const PermutationDefine& d : kPermutationDefines)
        defs.push_back({ d.name, perm.Has(d.bit) ? "1" : "0" });
    return defs;
}
