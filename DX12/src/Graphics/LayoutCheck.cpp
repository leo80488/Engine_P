#include "Graphics/LayoutCheck.h"

#include "Graphics/ShaderReflection.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Layout
{
namespace
{
    // Written once by Register before the first load, read by every loading
    // thread afterwards (shaders load lazily on graph recording workers).
    const Spec* g_specs = nullptr;
    uint32_t    g_count = 0;

    std::mutex            g_mutex;      // everything below
    std::set<std::string> g_reported;   // "block|shader|member": one ERROR each
    std::set<const Spec*> g_seen;       // specs whose proof-of-life line is out
    std::set<std::string> g_checked;    // "block|shader": the summary's shader counts
    uint32_t              g_mismatches = 0;

    struct Member
    {
        std::string name;
        uint32_t    offset = 0;
        uint32_t    size   = 0;
        bool        sized  = false;   // a structured-buffer member carries no size
    };

    // Is @p name one of the '|'-separated names in @p list?
    bool Named(const char* list, const std::string& name)
    {
        if (!list || name.empty()) return false;
        for (const char* p = list; *p;)
        {
            const char*  bar = std::strchr(p, '|');
            const size_t len = bar ? static_cast<size_t>(bar - p) : std::strlen(p);
            if (len == name.size() && std::strncmp(p, name.c_str(), len) == 0) return true;
            if (!bar) break;
            p = bar + 1;
        }
        return false;
    }

    // -layoutcheck_break=<block> (harness): the first member's EXPECTED offset
    // moves by 4, so the check has a mismatch to catch - the reverse arm that
    // proves a clean run is the check passing, not the check not running.
    bool Broken(const Spec& spec)
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const std::string s_block = Sys::CommandLine::Get().Get("-layoutcheck_break");
        return !s_block.empty() && s_block == spec.block;
#else
        (void)spec;
        return false;
#endif
    }

    void Report(const Spec& spec, const char* shader, const std::string& member, const char* fmt, ...)
    {
        const std::string key = std::string(spec.block) + '|' + shader + '|' + member;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_reported.insert(key).second) return;
        }
        char what[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(what, sizeof(what), fmt, args);
        va_end(args);
        LOG_ERROR("[LAYOUT] mismatch: %s (%s) in %s: %s", spec.block, spec.cppType, shader, what);
    }

    Result Compare(const char* shader, const Spec& spec, const std::vector<Member>& members,
                   uint32_t blockSize)
    {
        Result res;
        res.blocks = 1;
        const bool broken = Broken(spec);
        auto expected = [&](uint32_t i) { return spec.vars[i].offset + ((broken && i == 0) ? 4u : 0u); };

        for (const Member& m : members)
        {
            ++res.vars;
            int idx = -1;
            for (uint32_t i = 0; i < spec.count; ++i)
                if (Named(spec.vars[i].names, m.name)) { idx = static_cast<int>(i); break; }

            if (idx >= 0)
            {
                const Var&     v   = spec.vars[idx];
                const uint32_t off = expected(static_cast<uint32_t>(idx));
                if (m.offset != off || (m.sized && m.size != v.size))
                {
                    ++res.mismatches;
                    if (m.sized)
                        Report(spec, shader, m.name, "'%s' hlsl offset=%u size=%u, c++ offset=%u size=%u",
                               m.name.c_str(), m.offset, m.size, off, v.size);
                    else
                        Report(spec, shader, m.name, "'%s' hlsl offset=%u, c++ offset=%u",
                               m.name.c_str(), m.offset, off);
                }
                continue;
            }
            if (!m.name.empty() && m.name[0] == '_')   // padding by convention
            {
                if (m.offset + (m.sized ? m.size : 0u) > spec.cppSize)
                {
                    ++res.mismatches;
                    Report(spec, shader, m.name, "padding '%s' at %u..%u runs past the C++ struct (%u B)",
                           m.name.c_str(), m.offset, m.offset + m.size, spec.cppSize);
                }
                continue;
            }
            bool coincides = false;
            for (uint32_t i = 0; i < spec.count && !coincides; ++i)
                coincides = expected(i) == m.offset && (!m.sized || spec.vars[i].size == m.size);
            if (!coincides)
            {
                ++res.mismatches;
                Report(spec, shader, m.name, "'%s' (offset %u, size %u) is no C++ member's position - "
                       "a layout drift, or a name the spec should list", m.name.c_str(), m.offset, m.size);
            }
        }

        if (spec.kind == Kind::CBuffer)
        {
            const uint32_t rounded = (spec.cppSize + 15u) & ~15u;
            if (blockSize > rounded)
            {
                ++res.mismatches;
                Report(spec, shader, "<size>", "the cbuffer is %u bytes, the C++ struct %u (%u rounded)",
                       blockSize, spec.cppSize, rounded);
            }
        }
        else if (blockSize != spec.cppSize)
        {
            ++res.mismatches;
            Report(spec, shader, "<stride>", "the element stride is %u bytes, sizeof %u",
                   blockSize, spec.cppSize);
        }

        bool first = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            first = g_seen.insert(&spec).second;
            g_checked.insert(std::string(spec.block) + '|' + shader);
            g_mismatches += res.mismatches;
        }
        if (first)
            LOG_INFO("[LAYOUT] %s (%s, %u B) first checked against %s: %u members, %u mismatches%s",
                     spec.block, spec.cppType, spec.cppSize, shader, res.vars, res.mismatches,
                     broken ? " (-layoutcheck_break)" : "");
        return res;
    }

    bool Foreign(const Spec& spec, const std::vector<Member>& members)
    {
        for (const Member& m : members)
            if (Named(spec.foreign, m.name)) return true;
        return false;
    }

    std::vector<Member> MembersOf(const ShaderReflect::CBufferLayout& cb)
    {
        std::vector<Member> out;
        out.reserve(cb.vars.size());
        for (const ShaderReflect::CBufferVar& v : cb.vars)
            out.push_back({ v.name, v.offset, v.size, true });
        return out;
    }

    std::vector<Member> MembersOf(const ShaderReflect::StructElement& el)
    {
        std::vector<Member> out;
        out.reserve(el.members.size());
        for (const ShaderReflect::StructMember& m : el.members)
            out.push_back({ m.name, m.offset, 0u, false });
        return out;
    }

    void Add(Result& total, const Result& r)
    {
        total.blocks     += r.blocks;
        total.vars       += r.vars;
        total.mismatches += r.mismatches;
    }

    // Every block of @p r that @p spec claims; @p found says whether any did.
    Result CheckOne(const char* shader, const ShaderReflect::Reflection& r, const Spec& spec, bool& found)
    {
        Result total;
        found = false;
        if (spec.kind == Kind::CBuffer)
        {
            for (const ShaderReflect::CBufferLayout& cb : r.cbuffers)
            {
                if (cb.name != spec.block) continue;
                const std::vector<Member> members = MembersOf(cb);
                if (Foreign(spec, members)) continue;
                found = true;
                Add(total, Compare(shader, spec, members, cb.sizeBytes));
            }
        }
        else
        {
            for (const ShaderReflect::StructElement& el : r.elements)
            {
                if (el.type != spec.block) continue;
                const std::vector<Member> members = MembersOf(el);
                if (Foreign(spec, members)) continue;
                found = true;
                Add(total, Compare(shader, spec, members, el.stride));
            }
        }
        return total;
    }
} // namespace

void Register(const Spec* specs, uint32_t count)
{
    g_specs = specs;
    g_count = specs ? count : 0;
}

Result CheckRegistered(const char* shader, const ShaderReflect::Reflection& r)
{
    Result total;
    const char* name = shader ? shader : "<shader>";
    for (uint32_t i = 0; i < g_count; ++i)
    {
        bool found = false;
        Add(total, CheckOne(name, r, g_specs[i], found));
    }
    return total;
}

void LogSummary()
{
    std::string line;
    uint32_t    mismatches = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        mismatches = g_mismatches;
        for (uint32_t i = 0; i < g_count; ++i)
        {
            const std::string prefix = std::string(g_specs[i].block) + '|';
            uint32_t shaders = 0;
            for (const std::string& k : g_checked)
                if (k.compare(0, prefix.size(), prefix) == 0) ++shaders;
            char one[96];
            std::snprintf(one, sizeof(one), " %s=%u", g_specs[i].block, shaders);
            line += one;
        }
    }
    // name=value pairs: the layout_check pack bounds them (logLineFields).
    LOG_INFO("[LAYOUT] summary: specs=%u mismatches=%u shaders:%s", g_count, mismatches, line.c_str());
}

Result Check(const char* shader, const ShaderReflect::Reflection& r, const Spec& spec)
{
    const char* name = shader ? shader : "<shader>";
    bool found = false;
    Result res = CheckOne(name, r, spec, found);
    if (!found)
    {
        ++res.mismatches;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_mismatches;
        }
        Report(spec, name, "<block>", "the shader reflects no %s named '%s'",
               spec.kind == Kind::CBuffer ? "cbuffer" : "StructuredBuffer element", spec.block);
    }
    return res;
}

} // namespace Layout
