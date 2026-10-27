#include "Diagnostics/LuaDebugBindings.h"

#include "Audio/AudioClipSystem.h"
#include "Audio/AudioEngine.h"
#include "Diagnostics/FrameTiming.h"
#include "Graphics/GraphicsDX12.h"
#include "Renderer/Renderer.h"
#include "System/ThreadContext.h"
#include "Tools/CollisionMeshBaker.h"
#include "UI/DebugHud.h"

#include <sol/sol.hpp>

#include <string>

// ---- Debug.*: the stat-unit HUD's control + readout surface ----
// App owns the HUD, the audio engine and the window, the Renderer owns FrameStats; App hands them
// all over. The only way a scenario driver or the -control REPL can assert on these numbers: the
// HUD itself is pixels, which the harness reads worst.
void Diag::RegisterLuaDebugBindings(sol::state& state, Renderer& renderer, UI::DebugHud& debugHud,
                                    Audio::AudioEngine& audioEngine,
                                    Audio::AudioClipSystem& audioClipSys, GraphicsDX12& gfx)
{
    sol::state* lua = &state;
    Renderer* rp = &renderer;
    sol::table dbg = lua->create_table();
    // Debug.CollisionOverlay() -> { seen, prims, meshes, assets, skipped, triggers,
    // triangleLines, selectedDrawn }: what the collision overlay drew last frame
    // (Tools::CollisionMesh::LastWireStats). All zero while it is off.
    dbg.set_function("CollisionOverlay", [](sol::this_state ts) -> sol::table
    {
        const auto& w = Tools::CollisionMesh::LastWireStats();
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["seen"] = w.seen;           t["prims"] = w.prims;       t["meshes"] = w.meshes;
        t["assets"] = w.assets;       t["skipped"] = w.skipped;   t["triggers"] = w.triggers;
        t["triangleLines"] = w.triangleLines;
        t["selectedDrawn"] = w.selectedDrawn;
        return t;
    });
    dbg.set_function("SetHudLevel", [&debugHud](int n) { debugHud.SetLevel(n); });
    dbg.set_function("GetHudLevel", [&debugHud]() -> int { return int(debugHud.GetLevel()); });
    dbg.set_function("SetHudScale", [&debugHud](float s) { debugHud.SetUserScale(s); });
    // The current page's formatted rows, {label=, value=} per row - the HUD
    // is pixels, and a driver asserting on "stat gpu" needs the text.
    // Rows are formatted in the frame's PostRender half, so a level set this
    // frame shows next frame.
    dbg.set_function("HudRows", [&debugHud](sol::this_state ts) -> sol::table
    {
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        for (int i = 0; i < debugHud.RowCount(); ++i)
        {
            sol::table row = lv.create_table();
            row["label"] = std::string(debugHud.RowLabel(i));
            row["value"] = std::string(debugHud.RowValue(i));
            t[i + 1] = row;
        }
        return t;
    });
    // sol::this_state rather than a captured sol::state*: the closure
    // outlives this function, and borrowing the VM it is being called from
    // is the one form that cannot dangle.
    dbg.set_function("Stats", [&debugHud, &audioEngine, &audioClipSys, &gfx, rp](sol::this_state ts) -> sol::table
    {
        const auto& ft = Diag::FrameTiming::Get();
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        t["hudLevel"]      = int(debugHud.GetLevel());
        t["fps"]           = ft.fps;
        t["frameMs"]       = ft.frameMs;
        t["frameMsSmooth"] = ft.frameSm;
        t["cpuMs"]         = ft.cpuFrameMs;
        // The smoothed pair carries the frame >= cpu invariant; the raw pair describes adjacent frames and
        // inverts across a spike (see FrameTiming::frameMs).
        t["cpuMsSmooth"]   = ft.cpuSm;
        t["gameMsSmooth"]  = ft.gameSm;
        t["drawMsSmooth"]  = ft.drawSm;
        t["gpuBusyMsSmooth"] = ft.gpuSm;
        t["waitMs"]        = ft.gpuWaitMs;
        t["gameMs"]        = ft.gameMs;
        t["drawMs"]        = ft.drawMs;
        t["gpuBusyMs"]     = ft.gpuBusyMs;
        t["gpuOverlapMs"]  = ft.gpuOverlapMs;
        // ---- Game / render split rows ----
        // renderPhaseMs is the Render phase's cost (FrameTiming::renderPhaseMs). joinWaitValid is always
        // false (no executor joins a render thread): a driver must check it before asserting.
        t["renderPhaseMs"]       = ft.renderPhaseMs;
        t["renderPhaseMsSmooth"] = ft.renderPhaseSm;
        t["beginWaitMs"]         = ft.beginWaitMs;
        t["presentMs"]           = ft.presentMs;
        t["restMs"]              = ft.restMs;
        t["cpuPeriodMs"]         = ft.cpuPeriodMs;
        t["cpuPeriodMsSmooth"]   = ft.cpuPeriodSm;
        t["loopTopMs"]           = ft.loopTopMs;
        t["renderTimingFrame"]   = ft.renderTimingFrame;
        t["renderTimingValid"]   = ft.renderTimingValid;
        t["joinWaitMs"]          = ft.joinWaitMs;
        t["joinWaitValid"]       = ft.joinWaitValid;
        t["renderMode"]          = ft.renderMode == 3 ? "graph-overlap" : (ft.renderMode == 2 ? "graph" : (ft.renderMode == 0 ? "serial" : "threaded"));
        // ---- Owner-active rows ----
        // renderOwnerMs describes renderTimingFrame, like drawMs and renderPhaseMs; tailDrainMs /
        // blockingDrainMs / parkHookCount describe this frame's game half. Check renderOwnerValid before
        // asserting (the gpuValid contract).
        t["renderOwnerMs"]           = ft.renderOwnerMs;
        t["renderOwnerValid"]        = ft.renderOwnerValid;
        t["ownerContinuationsPump"]  = ft.ownerContinuationsPump;
        t["ownerContinuationsDrain"] = ft.ownerContinuationsDrain;
        t["tailDrainMs"]             = ft.tailDrainMs;
        t["blockingDrainMs"]         = ft.blockingDrainMs;
        t["parkHookCount"]           = ft.parkHookCount;
        t["splitViolations"]     = Sys::ThreadCtx::Violations().load();
        // Absence is a first-class answer here: a false valid flag means
        // "not measured", never "zero". A driver that asserts on the
        // number without checking the flag is asserting on a placeholder.
        t["gpuValid"]      = ft.gpuValid;
        t["gpuCalibrated"] = ft.gpuCalibrated;
        t["gpuAgeFrames"]  = ft.gpuAgeFrames;
        // XAudio2 source voices holding a slot. audioReady false = no audio
        // device (the engine failed to initialize): the count is then 0 by
        // construction, not a measurement.
        t["audioReady"]    = audioEngine.IsInitialized();
        t["audioVoices"]   = audioEngine.ActiveVoiceCount();
        // AudioClipSystem: the clips held and the references on them (one
        // per AudioSource holding a handle, one per fire-and-forget voice
        // still playing). Without a device AudioSystem is never bound, so
        // nothing acquires and both read 0 - check audioReady first.
        const Audio::AudioClipSystem::Counts clips = audioClipSys.GetCounts();
        t["audioClips"]    = clips.clips;
        t["audioClipRefs"] = clips.refs;
        if (rp)
        {
            const auto& fs = rp->GetFrameStats();
            t["drawPackets"]        = fs.drawPackets;
            t["staticInstSlots"]    = fs.staticInstSlots;
            t["dynamicInstances"]   = fs.dynamicInstances;
            t["lights"]             = fs.lights;
            t["visibleDrawCmds"]      = fs.visibleDrawCmds;
            t["visibleDrawCmdsValid"] = fs.visibleDrawCmdsValid;
            t["worldUIVerts"]         = fs.worldUIVerts;
            t["matBatches"]           = fs.matBatches;
            t["matBitsMismatches"]    = fs.matBitsMismatches;
            t["matSnapMismatches"]    = fs.matSnapMismatches;
            // Triangle census. triMainView means "CPU-submitted", not "visible", whenever cpuCullBypassed is
            // true: a driver asserting that a cull happened must read that flag first.
            t["triScene"]        = fs.triScene;
            t["triSceneValid"]   = fs.triSceneValid;
            t["triMainView"]     = fs.triMainView;
            t["cpuCullBypassed"] = fs.cpuCullBypassed;
            t["triOpaque"]       = fs.triOpaque;
            t["triTransparent"]  = fs.triTransparent;
            t["triWater"]        = fs.triWater;
            t["triOutline"]      = fs.triOutline;
            t["triShadowOnly"]   = fs.triShadowOnly;
            t["instancesEmitted"] = fs.instancesEmitted;
            // Post-cull GPU counters, ~kFrameSlots frames stale. Scope: main-camera opaque indirect draws
            // only, not reconcilable with triMainView. Check gpuVisStatsValid before asserting.
            t["gpuVisTriangles"]  = fs.gpuVisibleTriangles;
            t["gpuVisInstances"]  = fs.gpuVisibleInstances;
            t["gpuVisCommands"]   = fs.gpuVisibleCommands;
            t["gpuVisStatsValid"] = fs.gpuVisStatsValid;
            // Meshlet path: "vs" / "cpu" / "indirect", and the amplification shader's counters, a few
            // frames stale - check meshletStatsValid before asserting.
            t["meshletPath"]        = fs.meshletPath == 1 ? "cpu" : (fs.meshletPath == 2 ? "indirect" : "vs");
            t["meshletTested"]      = fs.meshletTested;
            t["meshletLodRejected"] = fs.meshletLodRejected;
            t["meshletCullFrustum"] = fs.meshletCullFrustum;
            t["meshletCullCone"]    = fs.meshletCullCone;
            t["meshletCullHiZ"]     = fs.meshletCullHiZ;
            t["meshletVisible"]     = fs.meshletVisible;
            t["meshletTris"]        = fs.meshletTris;
            t["meshletStatsValid"]  = fs.meshletStatsValid;
            t["meshletStatsFrame"]  = fs.meshletStatsFrame;

            // ---- Which frame the census above belongs to ----
            // Under -brslane the emit half that produces it runs on the render-owner lane, so a game-half
            // script reads a census exactly one frame old there and this frame's when the emit runs on main.
            // Exposed rather than inferred: "one frame old" and "torn mid-accumulation" look the same from Lua.
            //   statsFrame    - Engine.Frame() minus this is the lag.
            //   statsValid    - false before the first emit half published (frame 0 under the lane): the
            //                   counters are absent, not zero.
            //   statsFromLane - the publishing half ran on a Render thread.
            t["statsFrame"]       = fs.statsFrame;
            t["statsValid"]       = fs.statsValid;
            t["statsFromLane"]    = fs.statsFromLane;

            // ---- Bindless slot pools ----
            // Lets a scenario assert that per-entity slot recycling happens: a slot that is never freed looks,
            // in every other stat, like one never allocated.
            // Caveats (MeshDescriptorHeap): *Current starts at *Reserved (the persistent base), not 0;
            // *FreeList counts only slots reusable now - the kFrameCount-deferred pending buckets are not in
            // it, so a just-freed slot appears a few frames later.
            const auto ps = rp->GetMeshManager().GetDescriptorHeap().GetPoolStats();
            t["meshSlotsCurrent"]    = ps.meshCurrent;
            t["meshSlotsHighWater"]  = ps.meshHighWater;
            t["meshSlotsCapacity"]   = ps.meshCapacity;
            t["meshSlotsReserved"]   = ps.meshReserved;
            t["meshSlotsFreeList"]   = ps.meshFreeList;
            t["meshSlotOverflows"]   = ps.meshOverflows;
            t["bufferSlotsCurrent"]  = ps.bufferCurrent;
            t["bufferSlotsHighWater"]= ps.bufferHighWater;
            t["bufferSlotsCapacity"] = ps.bufferCapacity;
            t["bufferSlotsReserved"] = ps.bufferReserved;
            t["bufferSlotsFreeList"] = ps.bufferFreeList;
            t["bufferSlotOverflows"] = ps.bufferOverflows;
        }

        // ---- Descriptor heaps + command-list pool ----
        // Four scalars per managed heap (CBV_SRV_UAV, RTV, DSV, SAMPLER), so a scenario can bound
        // descriptor growth instead of waiting for a fault inside the driver. How to assert (see
        // DescriptorHeapAllocator.h):
        //   *Used       monotonic high-water cursor; FreeStatic does not decrement it, so it proves
        //               "bounded", never "recycled". Do not expect it to fall after a churn.
        //   *FreeBlocks the recycling signal: blocks on the uncoalesced free list. Also a cost:
        //               AllocateStatic scans them linearly under one global mutex, so an unbounded count
        //               taxes every allocation.
        //   *Capacity   the effective ceiling min(cpu staging, gpu static) as granted -
        //               CreateHeapWithFallback may have halved the request, so never assert the literal.
        //   *Exhausted  failed AllocateStatic calls. The 75% / 90% pressure notices watch CPU staging
        //               only, so for a heap whose GPU static region is smaller this is the only warning.
        {
            auto& dxs = gfx;
            const auto cs = dxs.GetCbvSrvUavHeapStats();
            t["descCbvSrvUavUsed"]       = cs.used;
            t["descCbvSrvUavCapacity"]   = cs.capacity;
            t["descCbvSrvUavExhausted"]  = cs.exhausted;
            t["descCbvSrvUavFreeBlocks"] = cs.freeBlocks;
            const auto rs = dxs.GetRtvHeapStats();
            t["descRtvUsed"]       = rs.used;
            t["descRtvCapacity"]   = rs.capacity;
            t["descRtvExhausted"]  = rs.exhausted;
            t["descRtvFreeBlocks"] = rs.freeBlocks;
            const auto ds = dxs.GetDsvHeapStats();
            t["descDsvUsed"]       = ds.used;
            t["descDsvCapacity"]   = ds.capacity;
            t["descDsvExhausted"]  = ds.exhausted;
            t["descDsvFreeBlocks"] = ds.freeBlocks;
            const auto ss = dxs.GetSamplerHeapStats();
            t["descSamplerUsed"]       = ss.used;
            t["descSamplerCapacity"]   = ss.capacity;
            t["descSamplerExhausted"]  = ss.exhausted;
            t["descSamplerFreeBlocks"] = ss.freeBlocks;
            // Peak is the most lists used in ONE frame; capacity is the
            // fixed array bound. GetCommandListAllocated() is a different
            // number (largest slot ever constructed) and is deliberately
            // not aliased onto either of these.
            t["cmdListPeak"]     = dxs.GetCommandListPeak();
            t["cmdListCapacity"] = GraphicsDX12::GetCommandListCapacity();
        }

        // ---- Material preview ----
        // previewEnabled is the kill switch's answer, not "a panel is open": a scenario testing the
        // disabled path must tell "off" from "on but idle", and both render 0 views.
        if (rp)
        {
            const auto& fs = rp->GetFrameStats();
            t["previewViewsRendered"] = fs.previewViewsRendered;
            t["previewViewDraws"]     = fs.previewViewDraws;
            t["previewEnabled"]       = rp->IsPreviewEnabled();
        }
        return t;
    });
    // Debug.SetDraw(category, on) -> bool: the editor's Debug menu toggles, by
    // name ("collision", "navsoup", "navmesh", "aabb", "frustum", "capsules",
    // "probes", "ddgi", "ppvolumes", "decals", "lightranges", "fog"). Turning
    // one on also turns the wireframe master switch on. false for an unknown
    // name - and always in Game.exe, which submits no debug visuals.
    // The editor replaces this stub (Editor/LuaDebugDrawBindings.cpp).
    dbg.set_function("SetDraw", [](const std::string&, bool) -> bool { return false; });
    (*lua)["Debug"] = dbg;
}
