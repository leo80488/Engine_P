#include "Diagnostics/LuaProfilerBindings.h"

#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerCollector.h"
#include "Diagnostics/ProfilerFrame.h"
#include "Diagnostics/ProfilerMemory.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/UploadStats.h"   // Profiler.Upload
#include "UI/GlyphStats.h"         // Profiler.Glyphs
#include "Graphics/PipelineStats.h" // Profiler.Spawn().pipeline
#include "Resource/PrefabSpawnQueue.h" // Profiler.Spawn().preload / .async
#include "ECS/Guid.h"                  // GuidStats (Profiler.Spawn().async)
#include "Renderer/GatherGateStats.h"   // Profiler.Spawn
#include "Resource/PrefabSpawn.h"       // Profiler.Spawn
#include "Resource/PrefabTemplate.h"    // Profiler.Spawn().template
#include "Scripting/LuaDoc.h"   // Profiler.MemoryTop, Profiler.Upload, Profiler.Glyphs, Profiler.Spawn

#include <sol/sol.hpp>

#include <cstdint>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>      // GetProcessMemoryInfo (Profiler.Process)
#include <tlhelp32.h>   // the thread count (Profiler.Process)

// ---- Profiler.*: the engine profiler's runtime surface ----
// SetLevel lets a scenario driver measure the profiler's own cost in one process: toggle it between
// two frame windows and gate p50(on) - p50(off). Frame() is the newest complete record - the
// previous frame (see ProfilerFrame.h).
void Diag::RegisterLuaProfilerBindings(sol::state& state, IGraphicsDevice* gfx)
{
    sol::state* lua = &state;
    sol::table prof = lua->create_table();
    prof.set_function("SetLevel", [](const std::string& name) -> bool
    {
        using Diag::Prof::Level;
        if      (name == "off")      Diag::Prof::SetLevel(Level::Off);
        else if (name == "stats")    Diag::Prof::SetLevel(Level::Stats);
        else if (name == "timeline") Diag::Prof::SetLevel(Level::Timeline);
        else if (name == "detail")   Diag::Prof::SetLevel(Level::Detail);
        else return false;
        return true;
    });
    prof.set_function("GetLevel", []() -> std::string
    { return Diag::Prof::LevelName(Diag::Prof::GetLevel()); });
    prof.set_function("Mark", [](const std::string& name) { Diag::Prof::Mark(name.c_str()); });
    // Memory right now: the GPU resource ledger per category {mb, count},
    // its totals, the reported gauges, and - from the newest record that
    // has the collector's DXGI sample - vramMB and the unattributed local
    // VRAM (DXGI minus the ledger's local categories).
    prof.set_function("Memory", [](sol::this_state ts) -> sol::table
    {
        namespace P = Diag::Prof;
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        P::GpuLedger L;
        P::ReadGpuLedger(L);
        sol::table cats = lv.create_table();
        for (int i = 0; i < P::kGpuMemCats; ++i)
        {
            if (!L.bytes[i] && !L.count[i]) continue;
            sol::table c = lv.create_table();
            c["mb"]    = double(L.bytes[i]) / 1048576.0;
            c["count"] = double(L.count[i]);
            cats[P::GpuMemCatName(static_cast<P::GpuMemCat>(i))] = c;
        }
        t["gpu"]        = cats;
        t["gpuTotalMB"] = double(L.totalBytes) / 1048576.0;
        t["gpuLocalMB"] = double(L.localBytes) / 1048576.0;
        t["placed"]     = double(L.placed);
        sol::table gauges = lv.create_table();
        for (int i = 0; i < P::kGauges; ++i)
        {
            double v = 0.0;
            if (P::ReadGauge(static_cast<P::Gauge>(i), v)) gauges[P::GaugeName(static_cast<P::Gauge>(i))] = v;
        }
        t["gauges"] = gauges;
        const uint64_t latest = P::LatestRecordedFrame();
        for (uint64_t k = 0; latest != ~0ull && k <= 8 && k <= latest; ++k)
        {
            P::FrameRecord r;
            if (!P::GetFrameRecord(latest - k, r) || !r.memValid) continue;
            t["vramMB"] = double(r.vramLocalBytes) / 1048576.0;
            t["unattributedLocalMB"] = double(r.vramLocalBytes) / 1048576.0 - double(L.localBytes) / 1048576.0;
            break;
        }
        return t;
    });
    // The largest live GPU objects, by the name the engine created them under
    // (Diag::Prof::ReadGpuObjects).
    LuaDoc::Fn(prof, "Profiler", "MemoryTop", [](sol::this_state ts, sol::optional<int> n) -> sol::table
    {
        sol::state_view lv(ts);
        std::vector<Diag::Prof::GpuObjectInfo> rows;
        Diag::Prof::ReadGpuObjects(rows, static_cast<size_t>((std::max)(0, n.value_or(30))));
        sol::table out = lv.create_table();
        int i = 0;
        for (const Diag::Prof::GpuObjectInfo& r : rows)
        {
            sol::table t = lv.create_table();
            t["name"]     = r.name;
            t["category"] = std::string(Diag::Prof::GpuMemCatName(r.cat));
            t["bytes"]    = static_cast<double>(r.bytes);
            t["mb"]       = static_cast<double>(r.bytes) / 1048576.0;
            out[++i] = t;
        }
        return out;
    },
        { .doc = "The largest live GPU objects (render-arch TL8.3): {name, category, bytes, mb}, largest first - "
                 "committed resources, heaps and shader-visible descriptor heaps; a placed resource's memory is its "
                 "heap's (a transient aliasing heap holds several textures). name is what the engine created it under "
                 "(a texture's or buffer's debug name), empty when it had none. The sum over all of them is "
                 "Profiler.Memory().gpuTotalMB. Empty in a build without the profiler.",
          .params = { { "n", "how many, largest first (default 30; 0 = all)", "integer", true } },
          .returns = "table" });
    // The process's own handles, threads and memory, for open / close leak checks - a window is two
    // reads and a subtraction, like Profiler.Upload.
    LuaDoc::Fn(prof, "Profiler", "Process", [](sol::this_state ts) -> sol::table
    {
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        DWORD handles = 0;
        if (::GetProcessHandleCount(::GetCurrentProcess(), &handles)) t["handles"] = static_cast<double>(handles);
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        {
            t["privateBytes"] = static_cast<double>(pmc.PrivateUsage);
            t["workingSetBytes"] = static_cast<double>(pmc.WorkingSetSize);
        }
        const DWORD pid = ::GetCurrentProcessId();
        const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            int threads = 0;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (::Thread32First(snap, &te))
                do
                {
                    if (te.th32OwnerProcessID == pid) ++threads;
                } while (::Thread32Next(snap, &te));
            ::CloseHandle(snap);
            t["threads"] = threads;
        }
        return t;
    },
        { .doc = "This process right now: {handles, threads, privateBytes, workingSetBytes} - its open kernel "
                 "handles, its threads, its committed private memory and its working set. A leak check is two "
                 "reads around the suspect work, after a warm-up round (a first use allocates for good).",
          .returns = "table" });
    // The CPU-blocking upload waits as running totals since boot (Graphics/UploadStats.h) - the same
    // numbers frames.jsonl carries on every row. Totals, so a window is two reads and a subtraction,
    // and two scripts reading it cannot disturb each other. The description below ships in Retail, so
    // it must not name a harness artifact (tools/strip_gate.py MARKERS; test_lua_doc_ratchet.py).
    LuaDoc::Fn(prof, "Profiler", "Upload", [](sol::this_state ts) -> sol::table
    {
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["flushes"]     = static_cast<double>(UploadStats::flushes.load(std::memory_order_relaxed));
        t["stallMs"]     = UploadStats::NsToMs(UploadStats::stallNs.load(std::memory_order_relaxed));
        t["imguiWaits"]  = static_cast<double>(UploadStats::imguiWaits.load(std::memory_order_relaxed));
        t["imguiWaitMs"] = UploadStats::NsToMs(UploadStats::imguiWaitNs.load(std::memory_order_relaxed));
        // The streaming upload (QueueTextureUpload).
        auto u = [](const std::atomic<uint64_t>& a)
        { return static_cast<double>(a.load(std::memory_order_relaxed)); };
        t["streamRequests"]      = u(UploadStats::streamRequests);
        t["streamBytes"]         = u(UploadStats::streamBytes);
        t["streamReplaced"]      = u(UploadStats::streamReplaced);
        t["streamCarriedOver"]   = u(UploadStats::streamCarriedOver);
        t["streamFallback"]      = u(UploadStats::streamFallback);
        t["streamDropped"]       = u(UploadStats::streamDropped);
        t["streamRejected"]      = u(UploadStats::streamRejected);
        t["streamStale"]         = u(UploadStats::streamStale);
        t["streamCopies"]        = u(UploadStats::streamCopies);
        t["streamRingHighWater"] = u(UploadStats::streamRingHighWater);
        // Which queue those flushes waited on.
        t["copyFlushes"]         = u(UploadStats::copyFlushes);
        t["directFlushes"]       = u(UploadStats::directFlushes);
        // The ImGui backend's uploads the streaming path took instead of waiting.
        t["imguiStreamed"]       = u(UploadStats::imguiStreamed);
        // The flushes the game thread waited in; the loader workers' own.
        t["flushesGame"]         = u(UploadStats::flushesGame);
        t["loaderFlushes"]       = u(UploadStats::loaderFlushes);
        t["loaderStallMs"]       = UploadStats::NsToMs(UploadStats::loaderStallNs.load(std::memory_order_relaxed));
        return t;
    },
        { .doc = "CPU-blocking upload waits since boot (Texture_Upload_Streaming_Plan.md TU0): "
                 "{flushes, stallMs, imguiWaits, imguiWaitMs}. flushes / stallMs are the device's "
                 "FlushUploadAndWait round trips on both upload lanes (a synchronous UpdateTexture, a "
                 "CreateTexture / CreateBuffer with initial data outside a batch, a batch's end) and the "
                 "wall time spent in them; imguiWaits / imguiWaitMs are the ImGui DX12 backend's own "
                 "texture uploads, which wait the same way (Editor only, 0 in Game). Running totals, never "
                 "reset: a window is the difference of two reads. The harness's per-frame record carries the "
                 "same four numbers on every row (uploadFlushes, uploadStallMs, imguiUploadWaits, "
                 "imguiUploadWaitMs). The stream* fields are the streaming upload's (QueueTextureUpload, TU1): "
                 "streamRequests, streamBytes, streamReplaced, streamCarriedOver, streamFallback, streamDropped, "
                 "streamRejected, streamStale (skipped: the texture was destroyed first), streamCopies (copies "
                 "recorded), streamRingHighWater (the most bytes one slot's ring has held - the one maximum). "
                 "copyFlushes / directFlushes (TU4) say which queue those flushes waited on: the COPY queue "
                 "for copies into resources just created with initial data (the wait is the copy itself), the "
                 "graphics queue for updates of existing resources, and for the creation copies too under "
                 "-upload_directqueue (the wait is the GPU's backlog); a flush that ran both counts in both. "
                 "imguiStreamed (TU5) counts the ImGui backend's texture uploads handed to the streaming path "
                 "instead of waited for (Editor only). flushesGame (Async_Prefab_Instantiate_Plan.md AP5) is the "
                 "part of flushes the game thread waited in; loaderFlushes / loaderStallMs are the loader "
                 "workers' own batches of creation copies (a prefab preload's buffers and textures), waited for "
                 "on those workers and counted in none of the fields above.",
          .returns = "table" });
    // The runtime prefab spawns (Resource/PrefabSpawn.h) and the two retained-gather gates a spawn
    // trips (Renderer/GatherGateStats.h) - the same numbers the per-frame record carries. Totals, so a
    // window is two reads and a subtraction.
    LuaDoc::Fn(prof, "Profiler", "Spawn", [](sol::this_state ts) -> sol::table
    {
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["spawns"] = static_cast<double>(Resource::SpawnStats::spawns.load(std::memory_order_relaxed));
        t["syncMs"] = static_cast<double>(Resource::SpawnStats::syncNs.load(std::memory_order_relaxed)) / 1.0e6;
        Resource::SpawnCensus c;
        if (Resource::SpawnStats::Last(c))
        {
            sol::table l = lv.create_table();
            l["path"]          = c.path;
            l["caller"]        = c.caller;
            l["frame"]         = static_cast<double>(c.frame);
            l["ms"]            = c.ms;
            l["ok"]            = c.ok;
            l["entities"]      = c.entities;
            l["coldMeshlib"]   = c.coldMeshlib;
            l["coldMaterials"] = c.coldMaterials;
            l["coldShaders"]   = c.coldShaders;
            l["coldTextures"]  = c.coldTextures;
            l["reads"]         = c.reads;
            l["skeletons"]     = c.skeletons;
            l["skelRegistry"]  = c.skelRegistry;
            l["templated"]     = c.templated;
            l["preloaded"]     = c.preloaded;
            t["last"] = l;
        }
        {
            const Resource::PrefabTemplate::Counters& k = Resource::PrefabTemplate::Stats();
            sol::table tp = lv.create_table();
            tp["fileHits"]              = static_cast<double>(k.fileHits);
            tp["fileMisses"]            = static_cast<double>(k.fileMisses);
            tp["skeletonReuses"]        = static_cast<double>(k.skeletonReuses);
            tp["skeletonRegistrations"] = static_cast<double>(k.skeletonRegistrations);
            tp["sharedParts"]           = static_cast<double>(k.sharedParts);
            tp["decodedParts"]          = static_cast<double>(k.decodedParts);
            t["template"] = tp;
        }
        auto gate = [&lv](const GatherGateStats::Gate& g) -> sol::table
        {
            sol::table r = lv.create_table();
            r["frame"]  = static_cast<double>(g.frame.load(std::memory_order_relaxed));
            r["full"]   = g.full.load(std::memory_order_relaxed) != 0;
            r["reason"] = g.reason.load(std::memory_order_relaxed);
            r["fulls"]  = static_cast<double>(g.fulls.load(std::memory_order_relaxed));
            r["runs"]   = static_cast<double>(g.runs.load(std::memory_order_relaxed));
            // The scene build's time on the gate's frame (raster only; absent
            // when it was not timed on that frame).
            if (g.buildFrame.load(std::memory_order_relaxed) == g.frame.load(std::memory_order_relaxed))
                r["ms"] = static_cast<double>(g.buildUs.load(std::memory_order_relaxed)) / 1000.0;
            return r;
        };
        t["gather"] = gate(GatherGateStats::cand);
        t["ddgi"]   = gate(GatherGateStats::ddgi);
        // The preload's running totals, and the pipeline-side counters a preloaded spawn's window must not
        // move.
        {
            const Resource::PrefabSpawnQueue::PreloadCounters& k = Resource::PrefabSpawnQueue::Stats();
            sol::table p = lv.create_table();
            p["requests"]  = static_cast<double>(k.requests);
            p["ready"]     = static_cast<double>(k.ready);
            p["failed"]    = static_cast<double>(k.failed);
            p["stale"]     = static_cast<double>(k.stale);
            p["steps"]     = static_cast<double>(k.steps);
            p["mainMs"]    = k.mainMs;
            p["maxTickMs"] = k.maxTickMs;
            p["maxTickMsNoBig"] = k.maxTickMsNoBig;
            p["bigSteps"]  = static_cast<double>(k.bigSteps);
            sol::table frames = lv.create_table();
            for (size_t i = 0; i < k.bigStepFrames.size(); ++i) frames[i + 1] = static_cast<double>(k.bigStepFrames[i]);
            p["bigStepFrames"] = frames;
            t["preload"] = p;
        }
        {
            const Resource::PrefabSpawnQueue::AsyncCounters& a = Resource::PrefabSpawnQueue::AsyncStats();
            sol::table as = lv.create_table();
            as["requests"]            = static_cast<double>(a.requests);
            as["commits"]             = static_cast<double>(a.commits);
            as["cancelled"]           = static_cast<double>(a.cancelled);
            as["failed"]              = static_cast<double>(a.failed);
            as["late"]                = static_cast<double>(a.late);
            as["lateMs"]              = a.lateMs;
            as["pending"]             = a.pending;
            as["guidsMintedInCommit"] = static_cast<double>(ECS::GuidStats::MintedInCommit());
            t["async"] = as;
        }
        {
            sol::table pl = lv.create_table();
            pl["shaderMisses"]   = static_cast<double>(PipelineStats::shaderMisses.load(std::memory_order_relaxed));
            pl["shaderCompiles"] = static_cast<double>(PipelineStats::shaderCompiles.load(std::memory_order_relaxed));
            pl["psoCreates"]     = static_cast<double>(PipelineStats::psoCreates.load(std::memory_order_relaxed));
            pl["psoCacheMisses"] = static_cast<double>(PipelineStats::psoCacheMisses.load(std::memory_order_relaxed));
            pl["texCreates"]     = static_cast<double>(UploadStats::textureCreates.load(std::memory_order_relaxed));
            pl["texCreatesGame"] = static_cast<double>(UploadStats::textureCreatesGame.load(std::memory_order_relaxed));
            t["pipeline"] = pl;
        }
        return t;
    },
        { .doc = "Runtime prefab spawns and the scene-cache gates they trip (Async_Prefab_Instantiate_Plan.md "
                 "AP0 / AP1): {spawns, syncMs, last, template, gather, ddgi}. spawns / syncMs: synchronous runtime spawns "
                 "(Engine.SpawnPrefab, Engine.SpawnUIPrefab, a Sequencer spawnable, a save game's respawn) and "
                 "their wall time since boot - running totals, a window is two reads. last: the newest spawn's "
                 "census {path, caller, frame, ms, ok, entities, coldMeshlib, coldMaterials, coldShaders, "
                 "coldTextures, reads, skeletons, skelRegistry, templated, preloaded} - which of its inputs were not "
                 "resident yet (absent before the first spawn). template: the spawn template's running totals "
                 "{fileHits, fileMisses, skeletonReuses, skeletonRegistrations, sharedParts, decodedParts} - inputs "
                 "served without a read, skeleton registrations reused or made, skinned parts that took a live shared "
                 "source without a decode or were decoded. gather / ddgi: the raster candidate cache's and the DDGI static "
                 "gather's last gate {frame, full, reason, fulls, runs, ms} - full = that frame re-gathered the whole "
                 "scene, reason = the bits that vetoed the patch (1 added, 2 removed, 4 other, 8 mat, "
                 "16 texUnsettled, 32 unresolved, 64 setSig, 128 pick, 256 continuity, 512 invalid, 1024 dissolve, "
                 "2048 surfaceCache, 4096 off), fulls / runs running totals, ms (gather only) the whole scene "
                 "build's wall time on that frame. preload (AP3, Engine.PreloadPrefab): {requests, ready, failed, "
                 "stale, steps, mainMs, maxTickMs, maxTickMsNoBig, bigSteps, bigStepFrames} - running totals of "
                 "the preloads, their main-thread steps and time, the dearest frame's share (maxTickMsNoBig: with "
                 "the texture promotions' share that frame, over the frames without a big step), and the listed "
                 "big steps (the material surface cache) with the frames they ran on (the last 16). pipeline: {shaderMisses, "
                 "shaderCompiles, psoCreates, psoCacheMisses, texCreates, texCreatesGame} - shader cache misses "
                 "and compiles on every thread, pipelines the driver built and pipelines any pass cache built "
                 "(driver or PSO library), texture creations on any thread and on the game thread: running "
                 "totals, a window is two reads. async (AP4, Engine.SpawnPrefabAsync): {requests, commits, "
                 "cancelled, failed, late, lateMs, pending, guidsMintedInCommit} - running totals of the "
                 "asynchronous spawns, the fixed-frame commits that had to finish synchronously and their time, "
                 "the requests not committed now, and the Guids drawn from the RNG while a commit ran (0: a "
                 "commit derives every identity from its request's seed).",
          .returns = "table" });
    // The font atlas's glyph bake as running totals since boot (UI/GlyphStats.h), read like
    // Profiler.Upload. The description ships in Retail too: no harness artifact names in it.
    LuaDoc::Fn(prof, "Profiler", "Glyphs", [](sol::this_state ts) -> sol::table
    {
        namespace GS = UI::GlyphStats;
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        auto u = [](const std::atomic<uint64_t>& a) { return static_cast<double>(GS::Load(a)); };
        t["reserved"]        = u(GS::reserved);
        t["rendered"]        = u(GS::rendered);
        t["renderedOffMain"] = u(GS::renderedOffMain);
        t["empty"]           = u(GS::empty);
        t["tofu"]            = u(GS::tofu);
        t["tofuReused"]      = u(GS::tofuReused);
        t["late"]            = u(GS::late);
        t["geomMismatches"]    = u(GS::geomMismatches);
        t["mainMs"]          = GS::NsToMs(GS::Load(GS::mainNs));
        t["workerMs"]        = GS::NsToMs(GS::Load(GS::workerNs));
        t["renderMs"]        = GS::NsToMs(GS::Load(GS::renderNs));
        t["renderMaxMs"]     = GS::NsToMs(GS::Load(GS::renderMaxNs));
        t["asyncCells"]      = u(GS::asyncCells);
        t["joinCells"]       = u(GS::joinCells);
        t["joinWaitMs"]      = GS::NsToMs(GS::Load(GS::joinWaitNs));
        t["joinWaitMaxMs"]   = GS::NsToMs(GS::Load(GS::joinWaitMaxNs));
        return t;
    },
        { .doc = "The font atlas's glyph bake since boot (Glyph_Bake_Async_Plan.md GB0): {reserved, rendered, "
                 "renderedOffMain, empty, tofu, tofuReused, late, geomMismatches, mainMs, workerMs, renderMs}. "
                 "rendered counts the cells of real glyphs rendered (FreeType's outline SDF or the per-glyph "
                 "8SSEDT fallback), renderedOffMain those a TaskSystem worker rendered, empty the glyphs with "
                 "nothing to draw (an advance and no cell), tofu the boxes drawn for codepoints no font of the "
                 "chain has, tofuReused those of them that copied the font's first box (every box of a font is "
                 "the same cell, so its distance field is computed once). mainMs is the "
                 "main thread's wall time in the bake (loading outlines, rendering, packing, waiting for workers), "
                 "workerMs the render time spent on workers, renderMs the render time of every cell and box on any "
                 "thread. reserved counts cells placed before their pixels were rendered, late the cells that "
                 "became visible a frame after they were asked for, geomMismatches the rendered cells whose size or "
                 "bearing differed from the reservation. Running totals, never reset: a window is the difference "
                 "of two reads. Under the next-frame contract (Glyph_Bake_Async_Plan.md GB3): asyncCells counts "
                 "the cells handed to background tasks at a flush, joinCells those the next flush found unclaimed "
                 "and rendered itself, joinWaitMs its wait for cells a task was still rendering; joinWaitMaxMs and "
                 "renderMaxMs are the longest single wait and the longest single cell's render (maxima, not "
                 "totals). The harness's per-frame record carries glyphRendered, glyphRenderedOffMain, "
                 "glyphMainMs, glyphWorkerMs and glyphLate on every row.",
          .returns = "table" });
    // Named capture windows: frames [now + 1, now at EndWindow] / the next
    // `frames` frames, in the same summary and trace files as
    // -profile_window. false = no collector (no -profile on the command line).
    prof.set_function("BeginWindow", [](const std::string& name) -> bool
    { return Diag::Prof::BeginNamedWindow(name); });
    prof.set_function("EndWindow", [](const std::string& name) -> bool
    { return Diag::Prof::EndNamedWindow(name); });
    // -profile_hitch at runtime: frames slower than `ms`, at most `max` more
    // (a fresh budget); ms <= 0 disarms. false = no collector.
    prof.set_function("SetHitchCapture", [](double ms, sol::optional<int> max) -> bool
    { return Diag::Prof::SetHitchCapture(static_cast<float>(ms), static_cast<uint32_t>(std::max(1, max.value_or(3)))); });
    prof.set_function("Capture", [](int frames, sol::optional<std::string> name) -> bool
    {
        return frames > 0 && Diag::Prof::RequestCapture(name.value_or(std::string()),
                                                        static_cast<uint32_t>(frames));
    });
#if defined(WITH_HARNESS) && WITH_HARNESS
    // A deliberate stall of the calling thread inside its own scope - the
    // positive control for hitch capture (tests/scenarios/profile_hitch).
    prof.set_function("DebugStall", [](double ms)
    {
        PROF_SCOPE("Profiler.DebugStall");
        const int64_t until = Diag::Prof::NowNs() + static_cast<int64_t>(ms * 1e6);
        while (Diag::Prof::NowNs() < until) YieldProcessor();
    });
    // A deliberate GPU leak - the positive control for the ledger's churn
    // check (tests/scenarios/profile_memory_churn): a DEFAULT buffer of
    // `mb` MB that nothing destroys before shutdown.
    prof.set_function("DebugLeakGpu", [gfx](double mb) -> bool
    {
        if (!gfx || !(mb > 0.0) || mb > 1024.0) return false;
        static std::vector<RHI::GPUBuffer> s_leaked;
        RHI::GPUBufferDesc d{};
        d.size  = static_cast<uint64_t>(mb * 1048576.0);
        d.usage = RHI::Usage::DEFAULT;
        RHI::GPUBuffer b;
        if (!gfx->CreateBuffer(d, b)) return false;
        s_leaked.push_back(b);
        return true;
    });
#endif
    auto recordTable = [](sol::state_view lv, const Diag::Prof::FrameRecord& r) -> sol::table
    {
        sol::table t = lv.create_table();
        t["frame"]            = static_cast<double>(r.frame);
        t["periodMs"]         = r.periodMs;
        t["level"]            = std::string(Diag::Prof::LevelName(r.level));
        t["P"]                = static_cast<int>(r.poolThreads);
        t["workers"]          = static_cast<int>(r.workerThreads);
        t["util"]             = r.util;
        t["wTotalMs"]         = r.wTotalMs;
        t["sMainMs"]          = r.sMainMs;
        t["sLaneMs"]          = r.sLaneMs;
        t["workerWorkMs"]     = r.workerWorkMs;
        t["poolSpinMs"]       = r.poolSpinMs;
        t["mainUntrackedMs"]  = r.mainUntrackedMs;
        t["mainWaitGpuMs"]    = r.mainWaitGpuMs;
        t["mainWaitJoinMs"]   = r.mainWaitJoinMs;
        t["mainWaitPacingMs"] = r.mainWaitPacingMs;
        // Absent, not zero, when unmeasured (the gpuValid contract).
        if (r.osPoolCpuMs >= 0.f) t["osPoolCpuMs"] = r.osPoolCpuMs;
        t["timelineValid"]    = r.timelineValid;
        if (r.timelineValid) t["allIdleMs"] = r.allIdleMs;
        t["droppedSuspend"]   = r.droppedSuspend;
        t["orphaned"]         = r.orphaned;
        t["crossThread"]      = r.crossThread;
        return t;
    };
    prof.set_function("Frame", [recordTable](sol::this_state ts) -> sol::object
    {
        Diag::Prof::FrameRecord r;
        if (!Diag::Prof::GetLatestFrameRecord(r)) return sol::lua_nil;
        return recordTable(sol::state_view(ts), r);
    });
    prof.set_function("FrameOf", [recordTable](sol::this_state ts, double frame) -> sol::object
    {
        Diag::Prof::FrameRecord r;
        if (frame < 0.0 || !Diag::Prof::GetFrameRecord(static_cast<uint64_t>(frame), r))
            return sol::lua_nil;
        return recordTable(sol::state_view(ts), r);
    });
    (*lua)["Profiler"] = prof;
}
