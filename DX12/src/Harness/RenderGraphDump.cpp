#include "Harness/RenderGraphDump.h"

// Self-guarded as well as ExcludedFromBuild in the vcxproj (as GameFlowDump is): this
// TU is gated on EnableHarness while one of its consumers (the `rg` verb) is gated on
// EnableControl.
#ifdef WITH_HARNESS

#include "Graphics/FormatNames.h"
#include "Graphics/GraphicsStruct.h"
#include "RenderGraph/FrameCapture.h"
#include "RenderGraph/ImportSpec.h"
#include "RenderGraph/SceneView.h"   // RG::ShowFlag / CullVolume names
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace Harness
{
namespace
{
    using Json    = nlohmann::json;
    using Snap    = RG::GraphSnapshot;
    using Capture = RG::FrameCapture;
    using Device  = GfxDiag::FrameSnapshot;

    constexpr int         kSchema      = 1;
    // Under ControlQuery::Limits::maxTotalLen (48 KB), leaving room for the
    // request log ControlChannel attaches to every reply.
    constexpr std::size_t kReplyBudget = 44 * 1024;

    // The state, access, edge, queue and edge-class names are the data types' own
    // (GraphSnapshot, GfxDiag::FrameSnapshot, FrameCapture), so the editor's Render Graph
    // panel prints the same words.

    // Index-safe names: a corrupt index is written as such, never dereferenced.
    std::string TexName(const Snap& s, uint32_t t)
    {
        return t < s.textures.size() ? s.textures[t].name : "<texture " + std::to_string(t) + ">";
    }
    std::string PassName(const Snap& s, uint32_t p)
    {
        return p < s.passes.size() ? s.passes[p].name : "<pass " + std::to_string(p) + ">";
    }
    // A resource with no debug name (buffers have none) by its address.
    std::string ResourceName(const Device& d, uint32_t r)
    {
        if (r == Device::kNone) return "<all UAVs>";
        if (r >= d.resources.size()) return "<resource " + std::to_string(r) + ">";
        const Device::Resource& x = d.resources[r];
        if (!x.name.empty()) return x.name;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "<%s 0x%llX>", x.buffer ? "buffer" : "texture",
                      static_cast<unsigned long long>(x.address));
        return buf;
    }
    std::string ListName(const Device& d, uint32_t slot)
    {
        if (slot < d.commandLists.size() && !d.commandLists[slot].name.empty())
            return d.commandLists[slot].name;
        return "cmd" + std::to_string(slot);
    }
    Json PassOrNull(const Capture& c, int32_t pass)
    {
        if (pass < 0 || !c.graph) return nullptr;
        return PassName(*c.graph, static_cast<uint32_t>(pass));
    }
    Json IndexOrNull(uint32_t v) { return v != Device::kNone ? Json(v) : Json(nullptr); }

    // ==== The graph ====

    Json Counts(const Snap& s)
    {
        Json c;
        c["passes"]     = s.passes.size();
        c["active"]     = s.RanCount();
        c["textures"]   = s.textures.size();
        c["backed"]     = s.BackedCount();
        c["edges"]      = s.edges.size();
        c["barriers"]   = s.BarrierCount();
        c["waits"]      = s.WaitCount();
        c["auxTails"]   = s.AuxTailCount();
        c["undeclared"] = s.UndeclaredCount();
        // The frame's DAG checks and the UAV barriers.
        c["findings"]    = s.findings.size();
        c["staleReads"]  = s.FindingCount(Snap::Check::StaleRead);
        c["inactiveProducers"] = s.FindingCount(Snap::Check::InactiveProducer);
        c["conflicts"]   = s.FindingCount(Snap::Check::ConflictInPass);
        c["clearAfterLive"] = s.FindingCount(Snap::Check::ClearAfterLive);
        c["stencilOverlaps"] = s.FindingCount(Snap::Check::StencilOverlap);
        c["uavPairs"]    = s.FindingCount(Snap::Check::UavNeedsBarrier);
        c["uavBarriers"] = s.UavBarrierCount();
        return c;
    }

    Json FindingJson(const Snap& s, const Snap::Finding& f)
    {
        Json j;
        j["check"]   = Snap::CheckName(f.check);
        j["pass"]    = PassName(s, f.pass);
        j["texture"] = f.texture != ~0u ? Json(TexName(s, f.texture)) : Json(nullptr);
        j["after"]   = f.other != ~0u ? Json(PassName(s, f.other)) : Json(nullptr);
        return j;
    }

    // RG::ShowFlag bits as their names, in enumerator order.
    Json ShowFlagNames(uint32_t bits)
    {
        Json names = Json::array();
        for (uint32_t b = 0; b < RG::kShowFlagCount; ++b)
            if (bits & (1u << b)) names.push_back(RG::ShowFlagName(static_cast<RG::ShowFlag>(1u << b)));
        return names;
    }

    Json Header(const Snap& s)
    {
        Json j;
        j["frame"]    = s.frame;
        j["sequence"] = s.sequence;
        j["width"]    = s.width;
        j["height"]   = s.height;
        // The scene view the graph drew ("view" is the rg verb's own key - which reply this
        // is).
        j["sceneView"] = Json{ { "name", s.viewName }, { "show", ShowFlagNames(s.viewShow) },
                               { "hidden", ShowFlagNames(~s.viewShow & RG::kShowAll) },
                               // where it looked
                               { "cull", Json{ { "kind", RG::CullVolumeKindName(
                                                     static_cast<RG::CullVolume::Kind>(s.viewCullKind)) },
                                               { "eye", { s.viewEye[0], s.viewEye[1], s.viewEye[2] } },
                                               { "viewBit", s.viewBit } } } };
        j["gpuFrame"] = s.gpuFrame >= 0 ? Json(s.gpuFrame) : Json(nullptr);
        j["counts"]   = Counts(s);
        return j;
    }

    Json Ms(float v) { return v >= 0.f ? Json(static_cast<double>(v)) : Json(nullptr); }

    Json EdgeJson(const Snap& s, const Snap::Dependency& e, bool withIndices)
    {
        Json j;
        if (withIndices) { j["from"] = e.from; j["to"] = e.to; }
        j[withIndices ? "fromPass" : "from"] = PassName(s, e.from);
        j[withIndices ? "toPass"   : "to"]   = PassName(s, e.to);
        j["texture"] = TexName(s, e.texture);
        j["kind"]    = Snap::EdgeName(e.kind);
        if (e.texture < s.textures.size() && s.textures[e.texture].planar)   // depth-stencil: the planes it covers
            j["planes"] = Snap::PlanesName(e.planes);
        return j;
    }

    Json PassJson(const Snap& s, const Snap::Pass& p)
    {
        Json j;
        j["index"]       = p.index;
        j["name"]        = p.name;
        j["active"]      = p.Ran();
        j["skip"]        = Snap::SkipName(p.skip);
        j["colorTarget"] = p.colorTarget ? p.colorTarget : "None";
        j["stage"]       = p.stage ? p.stage : "Unstaged";
        j["registered"]  = p.registered;   // the first sort's input
        if (p.showFlags) j["showFlags"] = ShowFlagNames(p.showFlags);   // what it needs shown
        Json ord = Json::array();
        for (const RG::OrderRule& r : p.order)
            ord.push_back(Json{ { "kind", RG::OrderKindName(r.kind) }, { "pass", r.pass }, { "why", r.why } });
        j["order"]       = std::move(ord);
        j["cl"]          = p.cl != ~0u ? Json(p.cl) : Json(nullptr);
        j["auxTailCl"]   = p.auxTailCl != ~0u ? Json(p.auxTailCl) : Json(nullptr);
        j["waits"]       = p.externalWaits;
        j["cpuMs"]       = p.Ran() ? Json(static_cast<double>(p.cpuMs)) : Json(nullptr);
        j["gpuMs"]       = Ms(p.gpuMs);
        j["unread"]      = p.unread;   // a cull candidate (report only)
        Json acc = Json::array();
        for (const Snap::PassAccess& a : p.accesses)
        {
            Json x;
            x["texture"] = TexName(s, a.texture);
            x["kind"]    = Snap::AccessName(a.kind);
            if (a.kind == Snap::Access::Manual && a.entryState != 0)
                x["entryState"] = Snap::StateName(a.entryState);
            if (a.kind == Snap::Access::Scoped)
                x["state"] = Snap::StateName(a.entryState);
            if (a.kind == Snap::Access::RenderTarget && a.load != 0)
                x["load"] = Snap::LoadName(a.load);
            if (a.plane >= 0)
            {
                x["plane"] = Snap::PlaneName(a.plane);
                if (a.plane == 1 && a.stencilMask != 0xFF) x["stencilMask"] = a.stencilMask;
            }
            acc.push_back(std::move(x));
        }
        j["accesses"] = std::move(acc);
        Json bar = Json::array();
        for (const Snap::Barrier& b : p.barriers)
        {
            Json x = { { "texture", TexName(s, b.texture) },
                       { "before",  Snap::StateName(b.before) },
                       { "after",   Snap::StateName(b.after) } };
            if (b.plane >= 0) x["plane"] = Snap::PlaneName(b.plane);
            bar.push_back(std::move(x));
        }
        j["barriers"] = std::move(bar);
        Json uav = Json::array();
        for (uint32_t t : p.uavBarriers) uav.push_back(TexName(s, t));
        j["uavBarriers"] = std::move(uav);
        Json und = Json::array();
        for (uint32_t t : p.undeclared) und.push_back(TexName(s, t));
        j["undeclared"] = std::move(und);
        return j;
    }

    Json TextureJson(const Snap::Texture& t, uint32_t index)
    {
        Json j;
        j["index"]        = index;
        j["name"]         = t.name;
        j["width"]        = t.width;
        j["height"]       = t.height;
        j["format"]       = RHI::FormatName(static_cast<RHI::Format>(t.format));
        j["arraySize"]    = t.arraySize;
        j["depth"]        = t.isDepth;
        j["uav"]          = t.isUAV;
        j["graphScope"]   = t.graphScope;
        j["residency"]    = t.imported ? "imported" : (t.onDemand ? "onDemand" : "always");
        if (t.imported)   // what the DAG assumes at entry, who transitions it
        {
            j["entry"]    = RG::ImportEntryName(static_cast<RG::ImportEntry>(t.importEntry));
            j["tracking"] = RG::ImportTrackingName(static_cast<RG::ImportTracking>(t.importTracking));
        }
        if (!t.deviceName.empty()) j["deviceName"] = t.deviceName;
        j["backed"]       = t.backed;
        j["initialState"] = Snap::StateName(t.initialState);
        j["finalState"]   = Snap::StateName(t.finalState);
        if (t.planar)   // the states above are the depth plane's
        {
            j["planar"]              = true;
            j["stencilInitialState"] = Snap::StateName(t.stencilInitialState);
            j["stencilFinalState"]   = Snap::StateName(t.stencilFinalState);
        }
        j["firstUse"]     = t.firstUse >= 0 ? Json(t.firstUse) : Json(nullptr);
        j["lastUse"]      = t.lastUse  >= 0 ? Json(t.lastUse)  : Json(nullptr);
        return j;
    }

    // ==== The device frame ====

    Json CoverageJson(const Capture& c)
    {
        const Capture::Coverage& n = c.coverage;
        Json j;
        j["frame"]           = c.frame;
        j["captureSequence"] = c.sequence;
        j["graph"]           = c.graph != nullptr;
        j["lists"]           = n.commandLists;
        j["batches"]         = n.batches;
        j["barriers"]        = n.barriers;
        j["native"]          = n.nativeBarriers;
        j["resources"]       = n.resources;
        j["observedEdges"]   = n.observedEdges;
        j["declaredEdges"]   = n.declaredEdges;
        j["coveredEdges"]    = n.coveredEdges;
        j["hiddenEdges"]     = n.hiddenEdges;
        j["readReadEdges"]   = n.readReadEdges;
        j["intraPassEdges"]  = n.intraPassEdges;
        j["crossQueueEdges"] = n.crossQueueEdges;
        j["unorderedEdges"]  = n.unorderedEdges;
        j["stateMismatches"] = n.stateMismatches;
        j["undeclaredResourceEdges"] = n.undeclaredResourceEdges;
        j["opaquePasses"]    = n.opaquePasses;
        j["manualBarriers"]  = n.manualBarriers;
        if (!c.ignoredPass.empty()) j["ignoredPass"] = c.ignoredPass;
        return j;
    }

    Json ListJson(const Capture& c, const Device::CommandList& l)
    {
        const Device& d = *c.device;
        Json j;
        j["slot"]  = l.slot;
        j["name"]  = ListName(d, l.slot);
        j["queue"] = Device::QueueName(l.queue);
        j["pass"]  = PassOrNull(c, l.slot < c.passOfList.size() ? c.passOfList[l.slot] : -1);
        Json wf = Json::array();
        for (uint32_t w : l.waitFor) wf.push_back(w);
        j["waitFor"]      = std::move(wf);
        j["submit"]       = IndexOrNull(l.submitIndex);
        j["batch"]        = IndexOrNull(l.batch);
        Json waits = Json::array();
        for (const Device::Wait& w : l.waits)
            waits.push_back({ { "on", w.on }, { "queue", Device::QueueName(w.queue) }, { "value", w.value } });
        j["waits"]        = std::move(waits);
        j["firstBarrier"] = l.firstBarrier;
        j["barriers"]     = l.barrierCount;
        return j;
    }

    Json BarrierJson(const Device& d, const Device::Barrier& b, uint32_t index)
    {
        Json j;
        j["index"]    = index;
        j["cl"]       = b.cl;
        j["list"]     = ListName(d, b.cl);
        j["kind"]     = Device::KindName(b.kind);
        j["path"]     = b.path == Device::Path::Native ? "native" : "device";
        j["resource"] = ResourceName(d, b.resource);
        j["resourceIndex"] = IndexOrNull(b.resource);   // two resources can share a name
        if (b.kind == Device::Kind::Transition)
        {
            j["before"]      = Device::D3DStateName(b.before);
            j["after"]       = Device::D3DStateName(b.after);
            j["beforeBits"]  = b.before;
            j["afterBits"]   = b.after;
            j["subresource"] = IndexOrNull(b.subresource);
        }
        else if (b.kind == Device::Kind::Aliasing)
            j["resourceAfter"] = ResourceName(d, b.resourceAfter);
        if (b.flags) j["flags"] = b.flags;
        return j;
    }

    Json ObservedJson(const Capture& c, const Capture::ObservedEdge& e)
    {
        const Device& d = *c.device;
        Json j;
        j["from"]       = e.from;
        j["to"]         = e.to;
        j["fromList"]   = ListName(d, e.from);
        j["toList"]     = ListName(d, e.to);
        j["fromPass"]   = PassOrNull(c, e.fromPass);
        j["toPass"]     = PassOrNull(c, e.toPass);
        j["resource"]   = ResourceName(d, e.resource);
        j["resourceIndex"] = e.resource;
        j["state"]      = Device::D3DStateName(e.state);
        j["class"]      = Capture::ClassName(e.kind);
        j["resourceDeclared"] = e.resourceDeclared;
        j["ordered"]    = e.ordered;
        j["crossQueue"] = e.crossQueue;
        j["barrier"]    = e.barrier;
        return j;
    }

    Json MismatchJson(const Capture& c, const Capture::Mismatch& m)
    {
        const Device& d = *c.device;
        Json j;
        j["barrier"] = m.barrier;
        if (m.barrier < d.journal.size())
        {
            const Device::Barrier& b = d.journal[m.barrier];
            j["list"]     = ListName(d, b.cl);
            j["resource"] = ResourceName(d, b.resource);
            j["before"]   = Device::D3DStateName(b.before);
        }
        j["expected"] = Device::D3DStateName(m.expected);
        j["leftBy"]   = ListName(d, m.lastList);
        return j;
    }

    Json Observed(const Capture& c)
    {
        Json out = Json::array();
        for (const Capture::ObservedEdge& e : c.observed) out.push_back(ObservedJson(c, e));
        return out;
    }

    Json Mismatches(const Capture& c)
    {
        Json out = Json::array();
        for (const Capture::Mismatch& m : c.mismatches) out.push_back(MismatchJson(c, m));
        return out;
    }

    // The device's table: lists in pool order, the submission, the batches.
    Json DeviceTable(const Capture& c)
    {
        const Device& d = *c.device;
        Json j;
        j["frame"] = d.frame;
        Json lists = Json::array();
        for (const Device::CommandList& l : d.commandLists) lists.push_back(ListJson(c, l));
        j["commandLists"] = std::move(lists);
        j["submitOrder"]  = d.submitOrder;
        Json batches = Json::array();
        for (uint32_t i = 0; i < d.batches.size(); ++i)
        {
            const Device::Batch& b = d.batches[i];
            batches.push_back({ { "index", i }, { "queue", Device::QueueName(b.queue) },
                                { "firstSubmit", b.firstSubmit }, { "count", b.count },
                                { "signal", b.signalValue } });
        }
        j["batches"] = std::move(batches);
        return j;
    }

    Json Journal(const Capture& c, const std::string* resource)
    {
        const Device& d = *c.device;
        Json out = Json::array();
        for (uint32_t i = 0; i < d.journal.size(); ++i)
        {
            const Device::Barrier& b = d.journal[i];
            if (resource && ResourceName(d, b.resource) != *resource &&
                (b.kind != Device::Kind::Aliasing || ResourceName(d, b.resourceAfter) != *resource))
                continue;
            out.push_back(BarrierJson(d, b, i));
        }
        return out;
    }

    Json ResourcesJson(const Device& d)
    {
        Json out = Json::array();
        for (uint32_t r = 0; r < d.resources.size(); ++r)
        {
            char addr[24];
            std::snprintf(addr, sizeof(addr), "0x%llX", static_cast<unsigned long long>(d.resources[r].address));
            out.push_back({ { "index", r }, { "name", ResourceName(d, r) },
                            { "buffer", d.resources[r].buffer }, { "address", addr } });
        }
        return out;
    }

    // ==== The whole capture ====

    Json Full(const Capture& c)
    {
        Json j;
        j["schema"]    = kSchema;
        j["generator"] = "rg";
        if (c.graph)
        {
            const Snap& s = *c.graph;
            j.update(Header(s));
            Json passes = Json::array();
            for (const Snap::Pass& p : s.passes) passes.push_back(PassJson(s, p));
            j["passes"] = std::move(passes);
            Json textures = Json::array();
            for (uint32_t t = 0; t < s.textures.size(); ++t) textures.push_back(TextureJson(s.textures[t], t));
            j["textures"] = std::move(textures);
            Json edges = Json::array();
            for (const Snap::Dependency& e : s.edges) edges.push_back(EdgeJson(s, e, true));
            j["edges"] = std::move(edges);
            Json findings = Json::array();
            for (const Snap::Finding& f : s.findings) findings.push_back(FindingJson(s, f));
            j["findings"] = std::move(findings);
            j["deriveUs"] = s.deriveUs;
        }
        else
        {
            j["frame"] = c.frame;
            j["graphMissing"] = true;   // the main graph did not execute that frame
        }
        j["coverage"] = CoverageJson(c);
        if (c.device)
        {
            Json dev = DeviceTable(c);
            dev["resources"] = ResourcesJson(*c.device);
            dev["journal"]   = Journal(c, nullptr);
            j["device"]          = std::move(dev);
            j["observedEdges"]   = Observed(c);
            j["stateMismatches"] = Mismatches(c);
        }
        return j;
    }

    std::string Dump(const Json& j, bool pretty)
    {
        // `replace`: a pass, texture or resource name is engine data and could
        // in principle carry invalid UTF-8; the object must still serialise.
        return j.dump(pretty ? 2 : -1, ' ', false, Json::error_handler_t::replace);
    }

    std::string Error(const char* code, const std::string& message, Json extra = Json::object())
    {
        Json j;
        j["ok"]      = false;
        j["verb"]    = "rg";
        j["error"]   = code;
        j["message"] = message;
        j.update(extra);
        return Dump(j, false);
    }

    // The header of every reply: the graph's when there is one, else the frame.
    Json ReplyHeader(const Capture& c)
    {
        if (c.graph) return Header(*c.graph);
        Json j;
        j["frame"] = c.frame;
        j["graphMissing"] = true;
        return j;
    }

    // A reply over the pipe budget goes to a file; the reply names it.
    std::string ReplyOrFile(Json reply, const Capture& c, const std::string& resultDir, const char* view)
    {
        std::string body = Dump(reply, false);
        if (body.size() <= kReplyBudget) return body;
        char name[64];
        std::snprintf(name, sizeof(name), "/rg_%llu_%s.json",
                      static_cast<unsigned long long>(c.graph ? c.graph->frame : c.frame), view);
        const std::string path = resultDir + name;
        const std::string pretty = Dump(reply, true);
        if (!Sys::WriteFileBytes(path, pretty.data(), pretty.size()))
            return Error("write_failed", "the reply (" + std::to_string(body.size()) +
                         " bytes) is over the pipe budget and " + path + " could not be written");
        Json j;
        j["ok"]    = true;
        j["verb"]  = "rg";
        j["view"]  = view;
        j["file"]  = path;
        j["bytes"] = pretty.size();
        j["note"]  = "over the pipe's reply budget: the whole reply is in `file`";
        j.update(ReplyHeader(c));
        return Dump(j, false);
    }

    Json NameList(const Snap& s, bool passes)
    {
        Json names = Json::array();
        if (passes) for (const Snap::Pass& p : s.passes)    names.push_back(p.name);
        else        for (const Snap::Texture& t : s.textures) names.push_back(t.name);
        return names;
    }

    Json ResourceNames(const Device& d)
    {
        Json names = Json::array();
        for (uint32_t r = 0; r < d.resources.size(); ++r) names.push_back(ResourceName(d, r));
        return names;
    }

    std::string Trim(const std::string& v)
    {
        const size_t b = v.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        const size_t e = v.find_last_not_of(" \t\r\n");
        return v.substr(b, e - b + 1);
    }

    const char* kUsage =
        "rg [summary | pass <name> | resource <name> | edges | frame | journal [<resource>] | full]";
} // namespace

std::string BuildRenderGraphJson(const RG::FrameCapture& capture, bool pretty)
{
    try { return Dump(Full(capture), pretty); }
    catch (const std::exception& e)
    {
        LOG_ERROR("RenderGraphDump: could not serialise the capture: %s", e.what());
        return "{\"schema\":1,\"generator\":\"rg\",\"error\":\"serialise_failed\"}";
    }
}

bool WriteRenderGraphDump(const RG::FrameCapture& capture, const std::string& utf8Path)
{
    const std::string j = BuildRenderGraphJson(capture, true);
    if (!Sys::WriteFileBytes(utf8Path, j.data(), j.size()))
    {
        LOG_ERROR("RenderGraphDump: could not write '%s'", utf8Path.c_str());
        return false;
    }
    return true;
}

std::string ServeRgVerb(const RG::FrameCapture& c, const std::string& payload, const std::string& resultDir)
{
    try
    {
        const std::string req  = Trim(payload);
        const size_t      sp   = req.find_first_of(" \t");
        const std::string view = req.substr(0, sp);
        const std::string arg  = sp == std::string::npos ? std::string() : Trim(req.substr(sp + 1));

        Json reply;
        reply["ok"]   = true;
        reply["verb"] = "rg";

        // The device side: answers with or without the graph.
        if (view == "frame")
        {
            if (!c.device) return Error("no_device", "the capture holds no device frame snapshot");
            reply["view"] = "frame";
            reply.update(ReplyHeader(c));
            reply["coverage"]        = CoverageJson(c);
            reply["device"]          = DeviceTable(c);
            reply["observedEdges"]   = Observed(c);
            reply["stateMismatches"] = Mismatches(c);
            return ReplyOrFile(std::move(reply), c, resultDir, "frame");
        }
        if (view == "journal")
        {
            if (!c.device) return Error("no_device", "the capture holds no device frame snapshot");
            reply["view"] = "journal";
            reply.update(ReplyHeader(c));
            if (!arg.empty())
            {
                bool known = false;
                for (uint32_t r = 0; r < c.device->resources.size() && !known; ++r)
                    known = ResourceName(*c.device, r) == arg;
                if (!known)
                    return Error("no_such_resource", "no resource named '" + arg + "' was barriered that frame",
                                 Json{ { "resources", ResourceNames(*c.device) } });
                reply["resource"] = arg;
            }
            reply["journal"] = Journal(c, arg.empty() ? nullptr : &arg);
            return ReplyOrFile(std::move(reply), c, resultDir, "journal");
        }
        if (view == "full")
        {
            char name[48];
            std::snprintf(name, sizeof(name), "/rg_%llu_full.json",
                          static_cast<unsigned long long>(c.graph ? c.graph->frame : c.frame));
            const std::string path = resultDir + name;
            const std::string body = BuildRenderGraphJson(c, true);
            if (!Sys::WriteFileBytes(path, body.data(), body.size()))
                return Error("write_failed", path + " could not be written");
            reply["view"]  = "full";
            reply["file"]  = path;
            reply["bytes"] = body.size();
            reply.update(ReplyHeader(c));
            return Dump(reply, false);
        }

        // The graph's views.
        const bool graphView = view.empty() || view == "summary" || view == "pass" ||
                               view == "resource" || view == "edges";
        if (!graphView)
            return Error("bad_request", "unknown view '" + view + "'", Json{ { "usage", kUsage } });
        if (!c.graph)
            return Error("no_graph", "the main render graph did not execute in the captured frame "
                         "(frame " + std::to_string(c.frame) + "); `rg frame` still answers");
        const Snap& s = *c.graph;

        if (view.empty() || view == "summary")
        {
            reply["view"] = "summary";
            reply.update(Header(s));
            Json passes = Json::array();
            for (const Snap::Pass& p : s.passes)
            {
                Json x;
                x["name"]   = p.name;
                x["active"] = p.Ran();
                if (p.Ran())
                {
                    x["barriers"] = p.barriers.size();
                    x["cpuMs"]    = static_cast<double>(p.cpuMs);
                    if (p.gpuMs >= 0.f) x["gpuMs"] = static_cast<double>(p.gpuMs);
                }
                else x["skip"] = Snap::SkipName(p.skip);
                passes.push_back(std::move(x));
            }
            reply["passes"]   = std::move(passes);
            reply["coverage"] = CoverageJson(c);
            return ReplyOrFile(std::move(reply), c, resultDir, "summary");
        }

        if (view == "pass")
        {
            for (uint32_t i = 0; i < s.passes.size(); ++i)
            {
                if (s.passes[i].name != arg) continue;
                reply["view"] = "pass";
                reply["frame"] = s.frame;
                reply["pass"]  = PassJson(s, s.passes[i]);
                Json in = Json::array(), out = Json::array();
                for (const Snap::Dependency& e : s.edges)
                {
                    if (e.to == i)   in.push_back(EdgeJson(s, e, false));
                    if (e.from == i) out.push_back(EdgeJson(s, e, false));
                }
                reply["edgesIn"]  = std::move(in);
                reply["edgesOut"] = std::move(out);
                Json fs = Json::array();   // what the DAG checks said about it
                for (const Snap::Finding& f : s.findings)
                    if (f.pass == i) fs.push_back(FindingJson(s, f));
                reply["findings"] = std::move(fs);
                if (c.device)
                {
                    // The handoffs the journal saw into and out of this pass's lists.
                    Json oin = Json::array(), oout = Json::array();
                    for (const Capture::ObservedEdge& e : c.observed)
                    {
                        if (e.toPass == static_cast<int32_t>(i))   oin.push_back(ObservedJson(c, e));
                        if (e.fromPass == static_cast<int32_t>(i)) oout.push_back(ObservedJson(c, e));
                    }
                    reply["observedIn"]  = std::move(oin);
                    reply["observedOut"] = std::move(oout);
                }
                return ReplyOrFile(std::move(reply), c, resultDir, "pass");
            }
            return Error("no_such_pass", "no pass is named '" + arg + "'",
                         Json{ { "passes", NameList(s, true) } });
        }

        if (view == "resource")
        {
            for (uint32_t t = 0; t < s.textures.size(); ++t)
            {
                if (s.textures[t].name != arg) continue;
                reply["view"]    = "resource";
                reply["frame"]   = s.frame;
                reply["texture"] = TextureJson(s.textures[t], t);
                Json uses = Json::array(), barriers = Json::array(), edges = Json::array();
                for (const Snap::Pass& p : s.passes)
                {
                    for (const Snap::PassAccess& a : p.accesses)
                        if (a.texture == t)
                            uses.push_back({ { "pass", p.name }, { "active", p.Ran() },
                                             { "kind", Snap::AccessName(a.kind) } });
                    for (const Snap::Barrier& b : p.barriers)
                        if (b.texture == t)
                            barriers.push_back({ { "pass", p.name }, { "before", Snap::StateName(b.before) },
                                                 { "after", Snap::StateName(b.after) } });
                }
                for (const Snap::Dependency& e : s.edges)
                    if (e.texture == t) edges.push_back(EdgeJson(s, e, false));
                reply["uses"]     = std::move(uses);
                reply["barriers"] = std::move(barriers);
                reply["edges"]    = std::move(edges);
                if (c.device)
                {
                    // Every barrier any list recorded on it - the graph's Phase A
                    // and everyone else's - and the handoffs they imply.
                    reply["journal"] = Journal(c, &arg);
                    Json obs = Json::array();
                    for (const Capture::ObservedEdge& e : c.observed)
                        if (ResourceName(*c.device, e.resource) == arg) obs.push_back(ObservedJson(c, e));
                    reply["observedEdges"] = std::move(obs);
                }
                return ReplyOrFile(std::move(reply), c, resultDir, "resource");
            }
            return Error("no_such_texture", "no graph texture is named '" + arg + "' (`rg journal <name>` "
                         "answers for any resource the frame barriered)",
                         Json{ { "textures", NameList(s, false) } });
        }

        // view == "edges"
        reply["view"]  = "edges";
        reply["frame"] = s.frame;
        Json edges = Json::array();
        for (const Snap::Dependency& e : s.edges) edges.push_back(EdgeJson(s, e, false));
        reply["edges"] = std::move(edges);
        if (c.device) reply["observedEdges"] = Observed(c);
        return ReplyOrFile(std::move(reply), c, resultDir, "edges");
    }
    catch (const std::exception& e)
    {
        return Error("internal", e.what());
    }
}

} // namespace Harness

#endif // WITH_HARNESS
