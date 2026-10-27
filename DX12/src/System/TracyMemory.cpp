// The global operator new/delete replacement, serving two consumers (two TUs replacing
// operator new would be a link error or an ODR violation):
//   * Tracy's memory window - when TRACY_ENABLE and the memory hooks are on (Profile|x64 or
//     /p:EnableTracy=true; /p:TracyMemHooks=false turns them off);
//   * the engine profiler's per-thread allocation counters (Diag::Prof::NoteAlloc /
//     NoteFree) - with WITH_PROFILER, i.e. all but Retail; one relaxed load while the
//     profiler level is off.
// With neither (Retail) the file compiles to nothing and the CRT's operator new is used.
// Behaviour matches the CRT: malloc / _aligned_malloc, bad_alloc on failure, the CRT debug
// heap in Debug.
// Covers every operator-new allocation in the exe, EngineCore, ImGui and DirectXTex. Not
// raw malloc users (Lua's lua_Alloc, FreeType, prebuilt .libs); the Working-Set plot in
// GraphicsDX12::EndFrame still shows their aggregate.
// TRACY_ON_DEMAND: allocations made before the profiler connects are not recorded, so
// their frees show as unmatched. For a full memory session build Profile with
// /p:TracyOnDemand=false and start tracy-capture before the editor.
// TRACY_MEM_CALLSTACK_DEPTH (e.g. 12) records a call stack per allocation, ~5-10x the
// cost of the stackless default; leave it 0 for everyday captures.
// For a timing capture consider /p:TracyMemHooks=false (Engine.Cpp.props): the hooks add a
// queue write per allocation, and an allocation-heavy frame can push Tracy into
// backpressure that slows every zone.

#ifndef ENGINE_TRACY_MEM_HOOKS
#define ENGINE_TRACY_MEM_HOOKS 1
#endif

#include "Diagnostics/Profiler.h"   // DIAG_PROFILER, Diag::Prof::NoteAlloc / NoteFree

#if defined(TRACY_ENABLE) && ENGINE_TRACY_MEM_HOOKS
#  define ENGINE_HEAP_TRACY 1
#else
#  define ENGINE_HEAP_TRACY 0
#endif

#if ENGINE_HEAP_TRACY || DIAG_PROFILER

#include <tracy/Tracy.hpp>

#include <cstdlib>
#include <new>

#ifndef TRACY_MEM_CALLSTACK_DEPTH
#define TRACY_MEM_CALLSTACK_DEPTH 0
#endif

#if !ENGINE_HEAP_TRACY
    #define ENGINE_TRACY_ALLOC(ptr, size) ((void)0)
    #define ENGINE_TRACY_FREE(ptr)        ((void)0)
#elif TRACY_MEM_CALLSTACK_DEPTH > 0
    #define ENGINE_TRACY_ALLOC(ptr, size) TracyAllocS(ptr, size, TRACY_MEM_CALLSTACK_DEPTH)
    #define ENGINE_TRACY_FREE(ptr)        TracyFreeS(ptr, TRACY_MEM_CALLSTACK_DEPTH)
#else
    #define ENGINE_TRACY_ALLOC(ptr, size) TracyAlloc(ptr, size)
    #define ENGINE_TRACY_FREE(ptr)        TracyFree(ptr)
#endif

namespace
{
    void* AllocReport(std::size_t count)
    {
        void* ptr = std::malloc(count);
        if (ptr) { ENGINE_TRACY_ALLOC(ptr, count); Diag::Prof::NoteAlloc(count); }
        return ptr;
    }

    void* AllocReportAligned(std::size_t count, std::size_t align)
    {
        void* ptr = _aligned_malloc(count, align);
        if (ptr) { ENGINE_TRACY_ALLOC(ptr, count); Diag::Prof::NoteAlloc(count); }
        return ptr;
    }

    void FreeReport(void* ptr)
    {
        if (!ptr) return;   // delete nullptr is legal - never report it
        ENGINE_TRACY_FREE(ptr);
        Diag::Prof::NoteFree();
        std::free(ptr);
    }

    void FreeReportAligned(void* ptr)
    {
        if (!ptr) return;
        ENGINE_TRACY_FREE(ptr);
        Diag::Prof::NoteFree();
        _aligned_free(ptr);
    }
}

// ==== Plain ====
void* operator new(std::size_t count)
{
    void* ptr = AllocReport(count);
    if (!ptr) throw std::bad_alloc{};
    return ptr;
}
void* operator new[](std::size_t count)
{
    void* ptr = AllocReport(count);
    if (!ptr) throw std::bad_alloc{};
    return ptr;
}
void* operator new(std::size_t count, const std::nothrow_t&) noexcept
{
    return AllocReport(count);
}
void* operator new[](std::size_t count, const std::nothrow_t&) noexcept
{
    return AllocReport(count);
}

void operator delete(void* ptr) noexcept                    { FreeReport(ptr); }
void operator delete[](void* ptr) noexcept                  { FreeReport(ptr); }
void operator delete(void* ptr, std::size_t) noexcept       { FreeReport(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept     { FreeReport(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept   { FreeReport(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { FreeReport(ptr); }

// ==== Over-aligned (DirectXMath SIMD types etc.) ====
// The compiler pairs aligned news with aligned deletes for well-formed code, so routing
// these through _aligned_malloc/_aligned_free is safe.
void* operator new(std::size_t count, std::align_val_t al)
{
    void* ptr = AllocReportAligned(count, static_cast<std::size_t>(al));
    if (!ptr) throw std::bad_alloc{};
    return ptr;
}
void* operator new[](std::size_t count, std::align_val_t al)
{
    void* ptr = AllocReportAligned(count, static_cast<std::size_t>(al));
    if (!ptr) throw std::bad_alloc{};
    return ptr;
}
void* operator new(std::size_t count, std::align_val_t al, const std::nothrow_t&) noexcept
{
    return AllocReportAligned(count, static_cast<std::size_t>(al));
}
void* operator new[](std::size_t count, std::align_val_t al, const std::nothrow_t&) noexcept
{
    return AllocReportAligned(count, static_cast<std::size_t>(al));
}

void operator delete(void* ptr, std::align_val_t) noexcept                  { FreeReportAligned(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept                { FreeReportAligned(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept    { FreeReportAligned(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept  { FreeReportAligned(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept   { FreeReportAligned(ptr); }
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { FreeReportAligned(ptr); }

#endif // ENGINE_HEAP_TRACY || DIAG_PROFILER
