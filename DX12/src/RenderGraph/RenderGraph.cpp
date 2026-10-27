#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/StencilBits.h"   // the stencil registry (CheckStencilOwners)
#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerGpu.h"   // BuildSnapshot's per-pass GPU time
#include "Graphics/DepthTrace.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/TransientAliasArena.h"   // the views' alias heaps
#include "Graphics/GpuMemoryLedger.h"       // their bytes are no view's
#include "System/Check.h"
#include "System/Log.h"
#include "System/TaskSystem.h"
#include "System/TaskGraph.h"
#include "System/CommandLine.h"
#include <chrono>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace RG
{
    // ==== LambdaRenderPass ====
    class LambdaRenderPass : public RenderPass
    {
    public:
        LambdaRenderPass(std::string name,
                         std::function<void(RenderGraphBuilder&)>  setupFn,
                         std::function<void(RHI::CommandList&)>    executeFn,
                         std::function<bool()>                     isActiveFn)
            : m_name(std::move(name))
            , m_setupFn(std::move(setupFn))
            , m_executeFn(std::move(executeFn))
            , m_isActiveFn(std::move(isActiveFn))
        {}

        const char* GetName() const override { return m_name.c_str(); }
        void Setup(RenderGraphBuilder& b) override { if (m_setupFn) m_setupFn(b); }
        void Init(IGraphicsDevice&)       override {}

        RHI::CommandList Execute(RHI::CommandList cl) override
        {
            if (m_executeFn) m_executeFn(cl);
            return cl;
        }

        // No predicate = always active. With one, it goes through the same
        // -passactiveoff escape as the class overrides, so one flag turns every
        // activity gate off.
        bool IsActive() const override
        {
            if (!m_isActiveFn) return true;
            return !PassActivityGatesEnabled() || m_isActiveFn();
        }

        uint32_t HandledEvents() const override { return m_events; }
        void     AddEvents(uint32_t events) { m_events |= events; }

    private:
        uint32_t                                  m_events = 0;   // RenderGraph::MarkViewSafe
        std::string                               m_name;
        std::function<void(RenderGraphBuilder&)>  m_setupFn;
        std::function<void(RHI::CommandList&)>    m_executeFn;
        std::function<bool()>                     m_isActiveFn;
    };

    // ==== -passactiveoff (see the declarations in RenderGraph.h) ====
    namespace
    {
        bool g_passActivityGates    = true;
        bool g_passActivityResolved = false;

        // DescribePasses and BuildSnapshot spell the builtin target the same way.
        const char* ColorTargetName(BuiltinTexture t)
        {
            switch (t)
            {
            case BuiltinTexture::SwapChainColor:        return "SwapChainColor";
            case BuiltinTexture::HdrSceneColor:         return "HdrSceneColor";
            case BuiltinTexture::HdrSceneColorPreserve: return "HdrSceneColorPreserve";
            default:                                    return "None";
            }
        }
    }

    bool PassActivityGatesEnabled()
    {
        if (!g_passActivityResolved)
        {
            g_passActivityGates    = !Sys::CommandLine::Get().Has("-passactiveoff");
            g_passActivityResolved = true;
        }
        return g_passActivityGates;
    }

    void SetPassActivityGatesEnabled(bool on)
    {
        PassActivityGatesEnabled();   // resolve first, so a restore is honest
        g_passActivityGates = on;
    }

    void RenderGraph::AddPass(Stage stage, std::unique_ptr<RenderPass> pass,
                              std::initializer_list<Order> order)
    {
        if (!pass) return;
        PassNode node;
        node.pass       = std::move(pass);
        node.stage      = stage;
        node.registered = m_registered++;
        // A constraint names the other pass and says why (RenderGraph/Order.h); an
        // Unstaged pass keeps its position, so it takes none.
        if (ENGINE_CHECK(stage < Stage::Count || order.size() == 0,
                         "RenderGraph::AddPass('%s'): an Unstaged pass keeps its position - its %zu order "
                         "constraint(s) are dropped", node.pass->GetName(), order.size()))
            for (const Order& o : order)
                if (ENGINE_CHECK(o.pass && *o.pass && o.why && *o.why,
                                 "RenderGraph::AddPass('%s'): an order constraint needs the other pass's name "
                                 "and a reason (RenderGraph/Order.h) - it is dropped", node.pass->GetName()))
                    node.order.push_back({ o.kind, o.pass, o.why });
        // A pass joins under the graph's current view.
        node.pass->m_shown = m_view.show.HasAll(node.pass->RequiredShowFlags());
        m_passes.push_back(std::move(node));
        m_compiled      = false;
        m_stagesChecked = false;
    }

    void RenderGraph::AddPass(std::unique_ptr<RenderPass> pass)
    {
        AddPass(Stage::Unstaged, std::move(pass));
    }

    void RenderGraph::AddPass(Stage stage, const char* name,
                              std::function<void(RenderGraphBuilder&)>  setupFn,
                              std::function<void(RHI::CommandList&)>    executeFn,
                              std::function<bool()>                     isActiveFn,
                              std::initializer_list<Order>              order)
    {
        AddPass(stage, std::make_unique<LambdaRenderPass>(
            name ? name : "", std::move(setupFn), std::move(executeFn),
            std::move(isActiveFn)), order);
    }

    void RenderGraph::AddPass(const char* name,
                              std::function<void(RenderGraphBuilder&)>  setupFn,
                              std::function<void(RHI::CommandList&)>    executeFn,
                              std::function<bool()>                     isActiveFn)
    {
        AddPass(Stage::Unstaged, name, std::move(setupFn), std::move(executeFn),
                std::move(isActiveFn));
    }

    void RenderGraph::MarkViewSafe(const char* name)
    {
        for (PassNode& n : m_passes)
            if (n.pass && name && std::strcmp(n.pass->GetName(), name) == 0)
                if (auto* lambda = dynamic_cast<LambdaRenderPass*>(n.pass.get()))
                {
                    lambda->AddEvents(PassLifecycle::kViewSafe);
                    return;
                }
        ENGINE_CHECK(false, "RenderGraph::MarkViewSafe: no lambda pass '%s'", name ? name : "?");
    }

    RGTextureHandle RenderGraph::CreateTexture(const char* name, const RGTextureDesc& desc,
                                               TextureResidency residency)
    {
        if (!ENGINE_CHECK(name != nullptr, "RenderGraph::CreateTexture: null name"))
            return {};
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_graphTextures.size()); ++i)
            if (m_graphTextures[i].name == name)
            {
                ENGINE_CHECK_ONCE(!m_graphTextures[i].imported,
                    "RenderGraph::CreateTexture('%s'): the name is an import (ImportTexture) - "
                    "the graph does not own it, and the handle returned is the import's", name);
                return { i };
            }

        uint32_t idx = static_cast<uint32_t>(m_graphTextures.size());
        GraphTexture gt;
        gt.name      = name;
        gt.desc      = desc;
        gt.residency = residency;
        m_graphTextures.push_back(std::move(gt));
        m_compiled = false;
        return { idx };
    }

    // ---- Imported textures (see the header) ----
    bool RenderGraph::ImportsEnabled()
    {
        static const bool s_enabled = !Sys::CommandLine::Get().Has("-rgimportoff");
        return s_enabled;
    }

    bool RenderGraph::PlanesEnabled()
    {
        static const bool s_enabled = !Sys::CommandLine::Get().Has("-rgplaneoff");
        return s_enabled;
    }

    RGTextureHandle RenderGraph::ImportTexture(const char* name, const RGTextureDesc& desc,
                                               const ImportSpec& spec)
    {
        if (!ENGINE_CHECK(name != nullptr, "RenderGraph::ImportTexture: null name"))
            return {};
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_graphTextures.size()); ++i)
            if (m_graphTextures[i].name == name)
            {
                ENGINE_CHECK_ONCE(m_graphTextures[i].imported,
                    "RenderGraph::ImportTexture('%s'): the graph already owns a texture of that "
                    "name (CreateTexture) - the handle returned is that texture's", name);
                return { i };
            }

        uint32_t idx = static_cast<uint32_t>(m_graphTextures.size());
        GraphTexture gt;
        gt.name     = name;
        gt.desc     = desc;
        gt.imported = true;
        gt.spec     = spec;
        m_graphTextures.push_back(std::move(gt));
        m_compiled = false;
        return { idx };
    }

    void RenderGraph::BindImport(RGTextureHandle h, const RHI::Texture& texture, const float clear[4])
    {
        if (!h.IsValid() || h.id >= m_graphTextures.size()) return;
        GraphTexture& gt = m_graphTextures[h.id];
        if (!ENGINE_CHECK_ONCE(gt.imported,
                "RenderGraph::BindImport: '%s' is not an import (ImportTexture)", gt.name.c_str()))
            return;
        gt.bound          = texture;
        gt.boundThisFrame = true;
        for (int i = 0; i < 4; ++i) gt.clear[i] = clear ? clear[i] : 0.f;
    }

    const float* RenderGraph::ImportClearValue(RGTextureHandle h) const
    {
        if (!h.IsValid() || h.id >= m_graphTextures.size() || !m_graphTextures[h.id].imported)
            return nullptr;
        return m_graphTextures[h.id].clear;
    }

    void RenderGraph::EnsurePhysicalTexture(RGTextureHandle h, IGraphicsDevice& gfx)
    {
        // Graph-scope handles only: they are the first m_graphTextures.size()
        // entries of m_textures, at the same index, on every Compile().
        if (!h.IsValid() || h.id >= m_graphTextures.size()) return;
        GraphTexture& gt = m_graphTextures[h.id];
        // Called every frame by a wanted producer; after the first call
        // Compile() owns the texture exactly as it owns an Always one - which
        // is also why a failed creation is logged once here and retried by the
        // next Compile(), not re-attempted every frame.
        if (gt.residency != TextureResidency::OnDemand || gt.demanded) return;
        gt.demanded = true;
        if (m_boundSet != 0) return;   // a secondary view's set takes it at its next bind (PlaceBoundSet)

        // Uncompiled or dirty: m_textures is about to be rebuilt, and that
        // Compile() backs every demanded texture. Creating it now would only
        // hand it one more texture to destroy.
        if (!m_compiled || h.id >= m_textures.size()) return;
        VirtualTexture& vt = m_textures[h.id];
        if (CreatePhysicalTexture(vt, gfx))
            LOG_INFO("RenderGraph: on-demand texture '%s' backed at %ux%ux%u",
                     vt.name.c_str(),
                     vt.desc.width  ? vt.desc.width  : m_physicalWidth,
                     vt.desc.height ? vt.desc.height : m_physicalHeight,
                     vt.desc.arraySize);
    }

    void RenderGraph::BindConstantBuffer(const char* name, const RHI::GPUBuffer& buffer)
    {
        // One string -> enum resolve per registration; Execute then moves raw
        // pointers only. The warn-once matters: the string fallback brings back the
        // per-frame hashing the slots exist to avoid.
        const RGSlot slot = RGSlotFromName(name);
        if (slot != RGSlot::Count)
        {
            m_cbSlotBindings[size_t(slot)] = &buffer;
            return;
        }
        static bool s_warnedCB = false;
        if (!s_warnedCB)
        {
            s_warnedCB = true;
            LOG_WARNING("RenderGraph::BindConstantBuffer: '%s' has no RGSlot — using cold string fallback", name);
        }
        m_cbFallback[name] = &buffer;
    }

    void RenderGraph::BindBuffer(const char* name, const RHI::GPUBuffer& buffer)
    {
        const RGSlot slot = RGSlotFromName(name);
        if (slot != RGSlot::Count)
        {
            m_bufferSlotBindings[size_t(slot)] = &buffer;
            return;
        }
        static bool s_warnedBuffer = false;
        if (!s_warnedBuffer)
        {
            s_warnedBuffer = true;
            LOG_WARNING("RenderGraph::BindBuffer: '%s' has no RGSlot — using cold string fallback", name);
        }
        m_bufferFallback[name] = &buffer;
    }

    void RenderGraph::SetDrawList(const std::vector<DrawPacket>* packets)
    {
        m_drawList = packets;
    }

    void RenderGraph::SetExternalWait(const char* passName, RHI::CommandList depCL)
    {
        if (!passName) return;
        // The key is a NAME with no compile-time link to the pass, and the re-homing
        // in Execute only rescues a name that exists but is inactive this frame - so
        // check it here, at the registration site, where the caller still is.
        bool known = false;
        for (const PassNode& node : m_passes)
        {
            if (node.pass && std::strcmp(node.pass->InstanceName(), passName) == 0)
            {
                known = true;
                break;
            }
        }
        ENGINE_CHECK_ONCE(known,
            "RenderGraph::SetExternalWait: no registered pass is named '%s' - the "
            "wait would be dropped silently", passName);
        // Stage rule (RenderGraph/Stage.h): the graph waits on async producers that
        // feed the lighting - the DDGI / TLAS promote on the compute queue - so a wait
        // lands no earlier than LightingInputs. Earlier, it would stall the GBuffer and
        // the shadow maps, the work that runs beside the compute queue, for no reader.
        for (const PassNode& node : m_passes)
            if (node.pass && std::strcmp(node.pass->InstanceName(), passName) == 0 &&
                node.stage != Stage::Unstaged)
                ENGINE_CHECK_ONCE(node.stage >= Stage::LightingInputs,
                    "RenderGraph::SetExternalWait: pass '%s' is in stage %s - an external wait lands no earlier "
                    "than LightingInputs, or the work before it stops overlapping the compute queue",
                    passName, StageName(node.stage));
        m_externalWaits[passName] = depCL;
    }

    // ---- Setup-phase API ----

    RGTextureHandle RenderGraph::DeclareTexture(const char* name, const RGTextureDesc& desc)
    {
        if (!ENGINE_CHECK(name != nullptr, "RenderGraph::DeclareTexture: null name"))
            return {};
        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end()) return { it->second };

        uint32_t idx = static_cast<uint32_t>(m_textures.size());
        VirtualTexture vt;
        vt.name = name;
        vt.desc = desc;
        m_textures.push_back(std::move(vt));
        m_nameToIndex[name] = idx;
        return { idx };
    }

    // A pass's import - an entry of this Compile's m_textures, like a
    // DeclareTexture one, but never created: its source is read at each Execute's
    // start.
    RGTextureHandle RenderGraph::DeclarePassImport(const char* name, const RGTextureDesc& desc,
                                                   const ImportSpec& spec)
    {
        if (!ENGINE_CHECK(name != nullptr, "RenderGraph::DeclarePassImport: null name"))
            return {};
        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end())
        {
            ENGINE_CHECK_ONCE(false,
                "RenderGraph: the pass import '%s' takes a name this graph already has (a second "
                "producer, or a graph texture) - the handle returned is the earlier one's", name);
            return { it->second };
        }
        ENGINE_CHECK_ONCE(spec.source != nullptr,
            "RenderGraph: the pass import '%s' has no source - nothing binds it", name);
        uint32_t idx = static_cast<uint32_t>(m_textures.size());
        VirtualTexture vt;
        vt.name     = name;
        vt.desc     = desc;
        vt.imported = true;
        vt.source   = spec.source;
        vt.entry    = spec.entry;
        vt.tracking = spec.tracking;
        if (spec.source) vt.texture = *spec.source;
        m_textures.push_back(std::move(vt));
        m_nameToIndex[name] = idx;
        return { idx };
    }

    RGTextureHandle RenderGraph::LookupTexture(const char* name) const
    {
        auto it = m_nameToIndex.find(name);
        if (it == m_nameToIndex.end()) return {};
        return { it->second };
    }

    const RHI::Texture* RenderGraph::GetPhysicalTexture(RGTextureHandle h) const
    {
        if (!h.IsValid() || h.id >= m_textures.size()) return nullptr;
        AuditPhysicalAccess(h.id);
        return InspectPhysicalTexture(h);
    }

    const RHI::Texture* RenderGraph::InspectPhysicalTexture(RGTextureHandle h) const
    {
        if (!h.IsValid() || h.id >= m_textures.size()) return nullptr;
        const VirtualTexture& vt = m_textures[h.id];
        return vt.texture.IsValid() ? &vt.texture : nullptr;
    }

    RHI::ResourceState RenderGraph::GetTextureState(RGTextureHandle h) const
    {
        if (!h.IsValid() || h.id >= m_textures.size())
            return RHI::ResourceState::UNDEFINED;
        return m_textures[h.id].currentState;
    }

    void RenderGraph::SetTextureState(RGTextureHandle h, RHI::ResourceState state)
    {
        if (!h.IsValid() || h.id >= m_textures.size()) return;
        VirtualTexture& vt = m_textures[h.id];
        vt.currentState = state;
        if (vt.planar) vt.stencilState = state;   // every plane
    }

    RHI::ResourceState RenderGraph::GetTextureState(RGTextureHandle h, Plane plane) const
    {
        if (!h.IsValid() || h.id >= m_textures.size()) return RHI::ResourceState::UNDEFINED;
        const VirtualTexture& vt = m_textures[h.id];
        return (plane == Plane::Stencil && vt.planar) ? vt.stencilState : vt.currentState;
    }

    void RenderGraph::SetTextureState(RGTextureHandle h, RHI::ResourceState state, Plane plane)
    {
        if (!h.IsValid() || h.id >= m_textures.size()) return;
        VirtualTexture& vt = m_textures[h.id];
        if (plane == Plane::Stencil && vt.planar) vt.stencilState = state;
        else                                      vt.currentState = state;
    }

    // Phase A's plane rule (transitionPlanes) for the users outside the graph,
    // recorded at once on their own list.
    void RenderGraph::MovePlanes(IGraphicsDevice& gfx, RHI::CommandList cl, RGTextureHandle h,
                                 RHI::ResourceState depthTarget, RHI::ResourceState stencilTarget)
    {
        if (!h.IsValid() || h.id >= m_textures.size()) return;
        AuditPhysicalAccess(h.id);
        VirtualTexture& vt = m_textures[h.id];
        if (!vt.texture.IsValid() || vt.OwnerTracked()) return;
        const bool apart = vt.planar && PlanesEnabled();
        if (!apart) stencilTarget = depthTarget;
        const RHI::ResourceState d = vt.currentState;
        const RHI::ResourceState s = apart ? vt.stencilState : d;
        const bool moveD = depthTarget   != RHI::ResourceState::UNDEFINED && d != depthTarget;
        const bool moveS = stencilTarget != RHI::ResourceState::UNDEFINED && s != stencilTarget;
        if (!apart || (moveD && moveS && d == s && depthTarget == stencilTarget))
        {
            if (!moveD) return;
            gfx.PushBarrier(RHI::GPUBarrier::Image(&vt.texture, d, depthTarget), cl);
            vt.currentState = depthTarget;
            if (vt.planar) vt.stencilState = depthTarget;
            return;
        }
        RHI::GPUBarrier b[2];
        uint32_t n = 0;
        if (moveD)
        {
            b[n++] = RHI::GPUBarrier::ImagePlane(&vt.texture, 0, d, depthTarget);
            vt.currentState = depthTarget;
        }
        if (moveS)
        {
            b[n++] = RHI::GPUBarrier::ImagePlane(&vt.texture, 1, s, stencilTarget);
            vt.stencilState = stencilTarget;
        }
        if (n) gfx.PushBarriers(b, n, cl);
    }

    // ---- The view this graph draws ----
    // Every pass's Shown() at once: the forward fog asks a fog pass before the
    // graph runs (Renderer::WriteFogApplyCB), the activity gate at Execute.
    void RenderGraph::SetView(const SceneView& view)
    {
        m_view = view;
        for (PassNode& node : m_passes)
            if (node.pass) node.pass->m_shown = view.show.HasAll(node.pass->RequiredShowFlags());
    }

    // ---- A texture's content right after one pass ----
    void RenderGraph::ArmTextureCopy(std::string texture, std::string afterPass, RHI::Texture* dst,
                                     RHI::ResourceState* dstState)
    {
        m_copyArm.armed     = true;
        m_copyArm.texture   = std::move(texture);
        m_copyArm.afterPass = std::move(afterPass);
        m_copyArm.handle    = {};
        m_copyArm.dst       = dst;
        m_copyArm.dstState  = dstState;
    }

    uint32_t RenderGraph::FindPass(const std::string& name) const
    {
        for (uint32_t i = 0; i < m_passes.size(); ++i)
            if (m_passes[i].pass && name == m_passes[i].pass->InstanceName()) return i;
        return ~0u;
    }

    const char* RenderGraph::PassName(uint32_t index) const
    {
        return index < m_passes.size() && m_passes[index].pass ? m_passes[index].pass->InstanceName() : nullptr;
    }

    // Its own list, chained after @p after: the texture to COPY_SRC and the copy
    // to COPY_DST, CopyResource, the copy to SHADER_RESOURCE and the texture back
    // to the planes' states - MovePlanes moves the graph's book with the device's.
    // Serial (Phase A, or Phase C after the tail). No pass owns it.
    RHI::CommandList RenderGraph::RecordTextureCopy(IGraphicsDevice& gfx, RHI::CommandList after,
                                                    const TextureCopyArm& arm)
    {
        VirtualTexture& vt = m_textures[arm.handle.id];
        RHI::CommandList cl = gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        GfxTrace::Label(cl, "RG.Capture");
        gfx.SetCommandListName(cl, "RG.Capture");
        if (after.IsValid()) gfx.AddCommandListDependency(cl, after);
        const uint32_t region = gfx.BeginGPUTimestampNoTracy(cl, "RG.Capture");
        gfx.BeginEventMarker(cl, "RG.Capture");

        const RHI::ResourceState depthWas   = vt.currentState;
        const RHI::ResourceState stencilWas = vt.planar ? vt.stencilState : vt.currentState;
        MovePlanes(gfx, cl, arm.handle, RHI::ResourceState::COPY_SRC, RHI::ResourceState::COPY_SRC);
        if (*arm.dstState != RHI::ResourceState::COPY_DST)
            gfx.PushBarrier(RHI::GPUBarrier::Image(arm.dst, *arm.dstState, RHI::ResourceState::COPY_DST), cl);
        gfx.CopyTexture(vt.texture, *arm.dst, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Image(arm.dst, RHI::ResourceState::COPY_DST,
                                               RHI::ResourceState::SHADER_RESOURCE), cl);
        *arm.dstState = RHI::ResourceState::SHADER_RESOURCE;
        MovePlanes(gfx, cl, arm.handle, depthWas, stencilWas);

        gfx.EndEventMarker(cl);
        gfx.EndGPUTimestampNoTracy(cl, region);
        m_copyResult.done = true;
        return cl;
    }

    std::vector<RenderGraph::PassInfo> RenderGraph::DescribePasses() const
    {
        auto names = [this](const std::vector<RGTextureHandle>& hs)
        {
            std::vector<std::string> out;
            out.reserve(hs.size());
            for (RGTextureHandle h : hs)
                out.push_back(h.IsValid() && h.id < m_textures.size() ? m_textures[h.id].name
                                                                      : std::string("<invalid>"));
            return out;
        };
        std::vector<PassInfo> out;
        out.reserve(m_passes.size());
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            const PassNode& n = m_passes[i];
            PassInfo p;
            p.name    = n.pass ? n.pass->InstanceName() : "<null>";
            p.ran     = i < m_activePasses.size() && m_activePasses[i] != 0;
            p.shown   = !n.pass || n.pass->Shown();
            p.colorTarget = ColorTargetName(n.colorTarget);
            p.stage       = StageName(n.stage);
            p.reads     = names(n.srvHandles);
            p.writes    = names(n.rtHandles);
            p.depth     = names(n.dsHandles);
            // DepthStencil is depth-stencil, a stencil SRV and a copy's source a read, a
            // copy's destination a write.
            for (const PassNode::DsvDecl& d : n.dsvDecls) p.depth.push_back(names({ d.h })[0]);
            for (RGTextureHandle h : n.stencilSrvHandles) p.reads.push_back(names({ h })[0]);
            for (const auto& c : n.copySrc) p.reads.push_back(names({ c.first })[0]);
            for (const auto& c : n.copyDst) p.writes.push_back(names({ c.first })[0]);
            p.readWrite = names(n.uavHandles);
            p.manual    = names(n.manualHandles);
            p.scoped    = names(n.scopedHandles);
            out.push_back(std::move(p));
        }
        return out;
    }

    std::vector<RenderGraph::TextureInfo> RenderGraph::DescribeTextures() const
    {
        std::vector<TextureInfo> out;
        out.reserve(m_textures.size());
        for (const VirtualTexture& vt : m_textures)
        {
            TextureInfo t;
            t.name    = vt.name;
            t.width   = vt.desc.width;
            t.height  = vt.desc.height;
            t.format  = static_cast<uint32_t>(vt.desc.format);
            t.arraySize = vt.desc.arraySize;
            t.isDepth = vt.desc.isDepth;
            t.isUAV   = vt.desc.isUAV;
            // A pass's import has no graph-scope entry: the virtual texture says it is
            // one.
            t.imported = vt.imported;
            t.entry    = vt.entry;
            t.tracking = vt.tracking;
            for (const GraphTexture& g : m_graphTextures)
                if (g.name == vt.name)
                {
                    t.onDemand = g.residency == TextureResidency::OnDemand;
                    t.demanded = g.demanded;
                    break;
                }
            out.push_back(std::move(t));
        }
        return out;
    }

    RGTextureHandle RenderGraph::FindTexture(const std::string& name) const
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_textures.size()); ++i)
            if (m_textures[i].name == name) return { i };
        return {};
    }

    bool RenderGraph::WrittenByLastExecute(RGTextureHandle h) const
    {
        if (!h.IsValid()) return false;
        auto has = [h](const std::vector<RGTextureHandle>& hs)
        {
            for (RGTextureHandle x : hs)
                if (x.id == h.id) return true;
            return false;
        };
        for (size_t i = 0; i < m_passes.size() && i < m_activePasses.size(); ++i)
        {
            if (!m_activePasses[i]) continue;
            const PassNode& n = m_passes[i];
            if (has(n.rtHandles) || has(n.dsHandles) || has(n.uavHandles) || has(n.manualHandles)) return true;
            for (const PassNode::DsvDecl& d : n.dsvDecls)
                if (d.h.id == h.id && (d.depth == Depth::Write || d.stencil == Stencil::Write)) return true;
            for (const auto& c : n.copyDst)
                if (c.first.id == h.id) return true;
        }
        return false;
    }

    void RenderGraph::RecordWrite(RGTextureHandle h, bool isDepth)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        if (isDepth) m_currentSetupNode->dsHandles.push_back(h);
        else
        {
            m_currentSetupNode->rtHandles.push_back(h);
            m_currentSetupNode->rtLoads.push_back(Load::Load);   // WriteRenderTarget loads
        }
        if (h.id < m_textures.size())
        {
            if (isDepth) m_textures[h.id].hasDSV = true;
            else         m_textures[h.id].hasRTV = true;
        }
    }

    // One declaration per texture: a second one (the shim and an explicit
    // RenderTarget naming the same import) keeps the first entry and takes a
    // Clear, the only load op that changes what Phase A does.
    void RenderGraph::RecordRenderTarget(RGTextureHandle h, Load load)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        PassNode& node = *m_currentSetupNode;
        for (size_t i = 0; i < node.rtHandles.size(); ++i)
            if (node.rtHandles[i].id == h.id)
            {
                if (load == Load::Clear && i < node.rtLoads.size()) node.rtLoads[i] = Load::Clear;
                return;
            }
        node.rtHandles.push_back(h);
        node.rtLoads.push_back(load);
        if (h.id < m_textures.size()) m_textures[h.id].hasRTV = true;
    }

    void RenderGraph::RecordScopedRead(RGTextureHandle h, RHI::ResourceState state)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        PassNode& node = *m_currentSetupNode;
        for (RGTextureHandle x : node.scopedHandles)
            if (x.id == h.id) return;
        node.scopedHandles.push_back(h);
        node.scopedStates.push_back(state);
    }

    void RenderGraph::RecordRead(RGTextureHandle h, bool history)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        m_currentSetupNode->srvHandles.push_back(h);
        // ReadHistory: the same SRV transition; the DAG reads it as last frame's
        // content.
        if (history) m_currentSetupNode->historyHandles.push_back(h);
        if (h.id < m_textures.size()) m_textures[h.id].hasSRV = true;
    }

    void RenderGraph::RecordUAV(RGTextureHandle h)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        m_currentSetupNode->uavHandles.push_back(h);
        if (h.id < m_textures.size()) m_textures[h.id].hasUAV = true;
    }

    // Explicit "this pass barriers this texture itself" marker. Does NOT add the
    // handle to any transition list - it only tells the access validator that the
    // omission is deliberate.
    void RenderGraph::RecordManualBarrier(RGTextureHandle h, RHI::ResourceState entryState)
    {
        if (!m_currentSetupNode || !h.IsValid()) return;
        auto& handles = m_currentSetupNode->manualHandles;
        auto& states  = m_currentSetupNode->manualEntryStates;
        for (size_t i = 0; i < handles.size(); ++i)
            if (handles[i].id == h.id)
            {
                // Re-declaring the same handle is a no-op except when the second call names
                // an entry state and the first did not: keep the stronger declaration.
                if (entryState != RHI::ResourceState::UNDEFINED && i < states.size())
                    states[i] = entryState;
                return;
            }
        handles.push_back(h);
        states.push_back(entryState);
    }

    // One DepthStencil declaration per texture per pass: a second keeps the first
    // and takes the stronger access of each plane.
    void RenderGraph::RecordDepthStencil(RGTextureHandle h, Depth depth, Stencil stencil, uint8_t stencilMask)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        PassNode& node = *m_currentSetupNode;
#if WITH_HARNESS
        // -rgtest_stencilwide=<pass> (harness): that pass's stencil declarations
        // claim the whole byte - the stencil registry's reverse arm: StencilOwner at
        // every Compile, and StencilOverlap on a frame where a writer of the widened
        // bits runs after a reader of them.
        static const std::string s_stencilWide = Sys::CommandLine::Get().Get("-rgtest_stencilwide");
        if (!s_stencilWide.empty() && stencil != Stencil::None && node.pass &&
            s_stencilWide == node.pass->InstanceName())
            stencilMask = 0xFF;
#endif
        for (PassNode::DsvDecl& d : node.dsvDecls)
            if (d.h.id == h.id)
            {
                d.depth = std::max(d.depth, depth);
                // A declaration that uses no stencil bit adds none (its mask is the default's).
                if (stencil != Stencil::None)
                    d.stencilMask = d.stencil == Stencil::None ? stencilMask
                                                               : static_cast<uint8_t>(d.stencilMask | stencilMask);
                d.stencil = std::max(d.stencil, stencil);
                return;
            }
        node.dsvDecls.push_back({ h, depth, stencil, stencilMask });
        if (h.id < m_textures.size()) m_textures[h.id].hasDSV = true;
    }

    void RenderGraph::RecordStencilRead(RGTextureHandle h)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        for (RGTextureHandle x : m_currentSetupNode->stencilSrvHandles)
            if (x.id == h.id) return;
        m_currentSetupNode->stencilSrvHandles.push_back(h);
        if (h.id < m_textures.size()) m_textures[h.id].hasSRV = true;
    }

    void RenderGraph::RecordCopy(RGTextureHandle h, Plane plane, bool dst)
    {
        if (!h.IsValid() || !m_currentSetupNode) return;
        auto& list = dst ? m_currentSetupNode->copyDst : m_currentSetupNode->copySrc;
        for (const auto& c : list)
            if (c.first.id == h.id && c.second == plane) return;
        list.emplace_back(h, plane);
    }

    void RenderGraph::SetBuiltinTarget(BuiltinTexture t)
    {
        if (!m_currentSetupNode) return;
        m_currentSetupNode->colorTarget = t;
        // The shim: an HDR builtin declares the HDR import (when the graph has one) -
        // HdrSceneColor clears it, Preserve loads it. Phase A still binds it, after
        // the barriers.
        if (t != BuiltinTexture::HdrSceneColor && t != BuiltinTexture::HdrSceneColorPreserve) return;
        const RGTextureHandle hdr = LookupTexture(kHdrSceneTexture);
        if (!hdr.IsValid() || hdr.id >= m_textures.size() || !m_textures[hdr.id].imported) return;
        RecordRenderTarget(hdr, t == BuiltinTexture::HdrSceneColor ? Load::Clear : Load::Load);
        m_currentSetupNode->hdrShim = hdr;
    }

    // ---- The first sort (RenderGraph/Order.h) ----
    // Registration order -> declaration order, once per pass-list change and
    // before anything reads a position (the instance names, the stage check,
    // Setup, the DAG, Phase A). The staged passes go stage-major; inside a
    // stage a Kahn sort over the constraints whose priority is (anchor,
    // registration): "After X" anchors a pass right after the latest-
    // registered X, "Before Y" (without an After) right before the earliest Y,
    // no constraint at its own registration - the insertion point a feature
    // got from where it was called. An Unstaged pass keeps its registration
    // position. A position that changes takes its per-pass data along (the
    // declarations, the undeclared-access record) or starts it over (the
    // record-cost hints, the CPU profile).
    void RenderGraph::SortPasses()
    {
        const size_t n = m_passes.size();
        std::vector<uint32_t> reg(n);   // the registration order, as current indices
        for (size_t i = 0; i < n; ++i) reg[i] = static_cast<uint32_t>(i);
        std::stable_sort(reg.begin(), reg.end(), [this](uint32_t a, uint32_t b)
                         { return m_passes[a].registered < m_passes[b].registered; });

        constexpr size_t kStages = static_cast<size_t>(Stage::Count);
        std::array<std::vector<uint32_t>, kStages> byStage;   // registration order inside each
        uint32_t staged = 0, constraints = 0, inverted = 0, cycles = 0, unknown = 0;
        Stage latest = static_cast<Stage>(0);
        for (uint32_t i : reg)
        {
            const Stage s = m_passes[i].stage;
            if (s >= Stage::Count) continue;   // Unstaged
            ++staged;
            constraints += static_cast<uint32_t>(m_passes[i].order.size());
            if (s < latest) ++inverted;
            else            latest = s;
            byStage[static_cast<size_t>(s)].push_back(i);
        }
        if (staged == 0) return;

        std::unordered_map<std::string, std::vector<uint32_t>> byName;
        for (size_t i = 0; i < n; ++i)
            byName[m_passes[i].pass->GetName()].push_back(static_cast<uint32_t>(i));

        std::vector<int64_t>               anchor(n, 0);
        std::vector<uint32_t>              indegree(n, 0);
        std::vector<std::vector<uint32_t>> succ(n);
        std::vector<uint32_t>              sortedStaged, ready, out;
        sortedStaged.reserve(staged);
        for (size_t k = 0; k < kStages; ++k)
        {
            const std::vector<uint32_t>& ids = byStage[k];
            if (ids.empty()) continue;
            for (uint32_t p : ids)
            {
                const PassNode& node = m_passes[p];
                int64_t after = -1, before = -1;   // the latest After target, the earliest Before target
                for (const OrderRule& r : node.order)
                {
                    const char* kind = OrderKindName(r.kind);
                    const auto it = byName.find(r.pass);
                    if (it == byName.end())
                    {
                        ++unknown;
                        ENGINE_CHECK(false, "RenderGraph: '%s' is ordered %s '%s', and no pass of this graph has "
                                     "that name (RenderGraph/Order.h) - the constraint is dropped",
                                     node.pass->GetName(), kind, r.pass.c_str());
                        continue;
                    }
                    // Its targets: the other STAGED passes of that name (an
                    // Unstaged one - a harness duplicate - keeps its position).
                    uint32_t targets = 0;
                    for (uint32_t t : it->second)
                    {
                        const PassNode& other = m_passes[t];
                        if (t == p || other.stage >= Stage::Count) continue;
                        ++targets;
                        if (other.stage != node.stage)
                        {
                            // Another stage: the stage order decides, and one that agrees holds already.
                            ENGINE_CHECK(r.kind == Order::Kind::After ? other.stage < node.stage
                                                                      : other.stage > node.stage,
                                         "RenderGraph: '%s' (stage %s) is ordered %s '%s' (stage %s), against the "
                                         "stage order - the constraint is dropped",
                                         node.pass->GetName(), StageName(node.stage), kind, r.pass.c_str(),
                                         StageName(other.stage));
                            continue;
                        }
                        const bool isAfter = r.kind == Order::Kind::After;
                        succ[isAfter ? t : p].push_back(isAfter ? p : t);
                        ++indegree[isAfter ? p : t];
                        if (isAfter)
                            after = std::max<int64_t>(after, other.registered);
                        else if (before < 0 || other.registered < before)
                            before = other.registered;
                    }
                    ENGINE_CHECK(targets > 0, "RenderGraph: '%s' is ordered %s '%s', which names no other staged "
                                 "pass (only itself, or an Unstaged pass, which keeps its position) - the "
                                 "constraint is dropped", node.pass->GetName(), kind, r.pass.c_str());
                }
                anchor[p] = after >= 0  ? 2 * after + 1
                          : before >= 0 ? 2 * before - 1
                          : 2 * static_cast<int64_t>(node.registered);
            }

            // Kahn: the ready pass with the lowest (anchor, registration) first.
            ready.clear();
            out.clear();
            for (uint32_t p : ids)
                if (indegree[p] == 0) ready.push_back(p);
            while (!ready.empty())
            {
                const auto first = std::min_element(ready.begin(), ready.end(), [&](uint32_t a, uint32_t b)
                {
                    return anchor[a] != anchor[b] ? anchor[a] < anchor[b]
                                                  : m_passes[a].registered < m_passes[b].registered;
                });
                const uint32_t p = *first;
                ready.erase(first);
                out.push_back(p);
                for (uint32_t q : succ[p])
                    if (--indegree[q] == 0) ready.push_back(q);
            }
            if (out.size() != ids.size())
            {
                std::string stuck;
                for (uint32_t p : ids)
                    if (indegree[p] != 0)
                        stuck += (stuck.empty() ? "'" : ", '") + std::string(m_passes[p].pass->GetName()) + "'";
                ENGINE_CHECK(false, "RenderGraph: the order constraints of stage %s make a cycle through %s "
                             "(RenderGraph/Order.h) - the stage keeps its registration order",
                             StageName(static_cast<Stage>(k)), stuck.c_str());
                ++cycles;
                out = ids;
            }
            sortedStaged.insert(sortedStaged.end(), out.begin(), out.end());
        }

        // The staged positions of the registration order take the sorted
        // staged passes; an Unstaged pass keeps its position.
        std::vector<uint32_t> perm(n);   // declaration position -> current index
        for (size_t pos = 0, next = 0; pos < n; ++pos)
            perm[pos] = m_passes[reg[pos]].stage < Stage::Count ? sortedStaged[next++] : reg[pos];

        // moved: the fewest staged passes that explain the new order - the
        // ones off a longest run of it that kept the registration order.
        const size_t m = sortedStaged.size();
        std::vector<uint32_t> run(m, 1);
        std::vector<size_t>   prev(m, SIZE_MAX);
        size_t best = 0;
        for (size_t a = 0; a < m; ++a)
        {
            for (size_t b = 0; b < a; ++b)
                if (m_passes[sortedStaged[b]].registered < m_passes[sortedStaged[a]].registered &&
                    run[b] + 1 > run[a])
                {
                    run[a]  = run[b] + 1;
                    prev[a] = b;
                }
            if (run[a] > run[best]) best = a;
        }
        std::vector<uint8_t> kept(m, 0);
        for (size_t a = best; a != SIZE_MAX; a = prev[a]) kept[a] = 1;
        std::vector<uint32_t> position(n);
        for (size_t pos = 0; pos < n; ++pos) position[perm[pos]] = static_cast<uint32_t>(pos);
        uint32_t moved = 0;
        for (size_t a = 0; a < m; ++a)
        {
            if (kept[a]) continue;
            ++moved;
            const PassNode& node = m_passes[sortedStaged[a]];
            std::string by;
            for (const OrderRule& r : node.order)
                by += std::string(by.empty() ? " - " : ", ") + OrderKindName(r.kind) + " " + r.pass;
            LOG_INFO("[RG_ORDER] moved '%s' (%s): registered #%u, declared #%u%s",
                     node.pass->GetName(), StageName(node.stage), node.registered,
                     position[sortedStaged[a]], by.c_str());
        }
        LOG_INFO("[RG_ORDER] passes=%zu staged=%u constraints=%u moved=%u inverted=%u cycles=%u unknown=%u",
                 n, staged, constraints, moved, inverted, cycles, unknown);

        PermutePasses(perm);
    }

    // A new order: @p perm[position] = the current index of the pass that goes
    // there. The per-pass data follows its pass (the declarations Setup built, the
    // undeclared-access record) or starts over (the record-cost hints, the CPU
    // profile); a vector sized for another pass list is rebuilt where it is used.
    void RenderGraph::PermutePasses(const std::vector<uint32_t>& perm)
    {
        const size_t n = m_passes.size();
        bool permuted = false;
        for (size_t pos = 0; pos < n && pos < perm.size() && !permuted; ++pos) permuted = perm[pos] != pos;
        if (!permuted || !ENGINE_CHECK(perm.size() == n, "RenderGraph::PermutePasses: %zu positions for %zu passes",
                                       perm.size(), n))
            return;
        auto follow = [&perm, n](auto& v)
        {
            if (v.size() != n) return;
            std::remove_reference_t<decltype(v)> sorted;
            sorted.reserve(n);
            for (uint32_t i : perm) sorted.push_back(std::move(v[i]));
            v = std::move(sorted);
        };
        follow(m_passes);
        follow(m_passAccesses);
#if RG_VALIDATE_ACCESS
        follow(m_undeclaredByPass);
        follow(m_reportedByPass);
#endif
        m_recordCostMs.clear();   // scheduling hints only; Execute grows them back
        m_cpuPassMs.clear();
    }

#if WITH_HARNESS
    namespace
    {
        // -rgschedule=decl | shuffle[:seed], read once.
        struct SchedulePolicy
        {
            bool     shuffle = false;
            uint32_t seed    = 1;
        };
        const SchedulePolicy& Schedule()
        {
            static const SchedulePolicy s_policy = []
            {
                SchedulePolicy p;
                const std::string v = Sys::CommandLine::Get().Get("-rgschedule", "decl");
                if (v == "decl") return p;
                if (v.rfind("shuffle", 0) == 0 && (v.size() == 7 || v[7] == ':'))
                {
                    p.shuffle = true;
                    if (v.size() > 8) p.seed = static_cast<uint32_t>(std::strtoul(v.c_str() + 8, nullptr, 10));
                    return p;
                }
                ENGINE_CHECK(false, "-rgschedule=%s: the policies are decl and shuffle[:seed] - decl it is",
                             v.c_str());
                return p;
            }();
            return s_policy;
        }
    }
#endif

    // ---- The second sort ----
    // Declaration order -> execution schedule, once per pass-list change, with
    // the declarations Setup just made. decl (the default) is the identity.
    // shuffle[:seed] (harness) takes, inside each stage, a random order the
    // static DAG (every pass treated as running, its edges from the declarations
    // in declaration order) and the order constraints allow: a diagnostic, not an
    // optimisation. Where the picture then differs from decl's, the stage has an
    // order its declarations do not say - add an order constraint. Logs
    // [RG_SCHEDULE] policy= seed= edges= moved=.
    void RenderGraph::ApplySchedule()
    {
#if WITH_HARNESS
        const size_t n = m_passes.size();
        uint32_t staged = 0;
        for (const PassNode& node : m_passes)
            if (node.stage < Stage::Count) ++staged;
        if (staged == 0) return;
        const SchedulePolicy& policy = Schedule();
        if (!policy.shuffle)
        {
            LOG_INFO("[RG_SCHEDULE] policy=decl passes=%zu moved=0", n);
            return;
        }

        // The edges inside a stage: the static DAG's and the constraints'
        // (every other staged pass of the name, as the first sort took them).
        std::vector<std::vector<uint32_t>> succ(n);
        std::vector<uint32_t> indegree(n, 0);
        uint32_t edges = 0;
        auto edge = [&](uint32_t from, uint32_t to)
        {
            if (from == to || from >= n || to >= n) return;
            const Stage s = m_passes[from].stage;
            if (s >= Stage::Count || m_passes[to].stage != s) return;
            succ[from].push_back(to);
            ++indegree[to];
            ++edges;
        };
        const std::vector<uint8_t> all(n, 1);
        Dag staticDag;
        DagScratch scratch;
        DeriveDag(m_passAccesses, all, m_textures.size(), staticDag, scratch, &m_importFresh, &m_planar);
        for (const GraphSnapshot::Dependency& e : staticDag.edges) edge(e.from, e.to);
        std::unordered_map<std::string, std::vector<uint32_t>> byName;
        for (size_t i = 0; i < n; ++i)
            byName[m_passes[i].pass->GetName()].push_back(static_cast<uint32_t>(i));
        for (uint32_t p = 0; p < n; ++p)
            for (const OrderRule& r : m_passes[p].order)
                if (const auto it = byName.find(r.pass); it != byName.end())
                    for (uint32_t t : it->second)
                    {
                        if (r.kind == Order::Kind::After) edge(t, p);
                        else                              edge(p, t);
                    }

        // Kahn per stage, a seeded pick among the ready passes (a fixed LCG:
        // every build deals alike); the stage's positions take the result.
        uint32_t state = policy.seed;
        std::vector<uint32_t> perm(n), slots, ready, out;
        for (size_t i = 0; i < n; ++i) perm[i] = static_cast<uint32_t>(i);
        for (size_t k = 0; k < static_cast<size_t>(Stage::Count); ++k)
        {
            slots.clear();
            for (uint32_t i = 0; i < n; ++i)
                if (m_passes[i].stage == static_cast<Stage>(k)) slots.push_back(i);
            if (slots.size() < 2) continue;
            ready.clear();
            out.clear();
            for (uint32_t i : slots)
                if (indegree[i] == 0) ready.push_back(i);
            while (!ready.empty())
            {
                state = state * 1664525u + 1013904223u;
                const size_t pick = (state >> 8) % ready.size();
                const uint32_t p = ready[pick];
                ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(pick));
                out.push_back(p);
                for (uint32_t q : succ[p])
                    if (--indegree[q] == 0) ready.push_back(q);
            }
            if (!ENGINE_CHECK(out.size() == slots.size(), "-rgschedule=shuffle: the edges of stage %s make a cycle "
                              "(a dropped constraint's?) - the stage keeps its declaration order",
                              StageName(static_cast<Stage>(k))))
                continue;
            for (size_t j = 0; j < slots.size(); ++j) perm[slots[j]] = out[j];
        }
        uint32_t moved = 0;
        for (size_t i = 0; i < n; ++i)
            if (perm[i] != i) ++moved;
        LOG_INFO("[RG_SCHEDULE] policy=shuffle seed=%u passes=%zu staged=%u edges=%u moved=%u",
                 policy.seed, n, staged, edges, moved);
        PermutePasses(perm);
#endif
    }

    // ---- Stage order (RenderGraph/Stage.h) ----
    // The graph runs in declaration order, so a staged graph's declaration
    // order must BE its stage order: a tagged pass may not follow a pass of a
    // later stage. The first sort (SortPasses) makes it so and this checks its
    // result: a failure is the sort's bug. Unstaged passes (the preview graphs,
    // the harness's test passes) take no part. [RG_STAGES] is the census the
    // rg_snapshot pack checks; a graph with no tagged pass prints none.
    void RenderGraph::CheckStageOrder()
    {
        std::array<uint32_t, static_cast<size_t>(Stage::Count)> perStage{};
        uint32_t staged = 0, outOfOrder = 0;
        size_t top = m_passes.size();   // the tagged pass of the latest stage so far
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            const Stage s = m_passes[i].stage;
            if (s >= Stage::Count) continue;   // Unstaged
            ++staged;
            ++perStage[static_cast<size_t>(s)];
            if (top < m_passes.size() &&
                !ENGINE_CHECK(s >= m_passes[top].stage,
                              "RenderGraph: pass #%zu '%s' (stage %s) is declared after pass #%zu '%s' "
                              "(stage %s) - the graph runs in declaration order, so a pass may not go "
                              "back to an earlier stage",
                              i, m_passes[i].pass->InstanceName(), StageName(s),
                              top, m_passes[top].pass->InstanceName(), StageName(m_passes[top].stage)))
            {
                ++outOfOrder;
                continue;
            }
            top = i;
        }
        if (staged == 0) return;
        std::string census;
        for (size_t k = 0; k < perStage.size(); ++k)
            if (perStage[k])
                census += " " + std::string(StageName(static_cast<Stage>(k))) + "=" + std::to_string(perStage[k]);
        LOG_INFO("[RG_STAGES] passes=%zu staged=%u outOfOrder=%u%s",
                 m_passes.size(), staged, outOfOrder, census.c_str());
    }

    // ---- Compile ----

    void RenderGraph::Compile(IGraphicsDevice& gfx)
    {
#if WITH_HARNESS
        // ---- -rgtest_uavpair: the UAV-after-UAV proof (tests/unit/test_rg_dag.py) ----
        // Two passes that use one small texture as a UAV and do nothing else - a
        // pair no real pass produces. Phase A must give the second a UAV barrier
        // (and must not under -rguavbarrieroff); the frame capture's barrier journal
        // shows which. Appended to the main graph once (passes and graph textures
        // persist across the resize recompile), here rather than in Renderer::Compile
        // so the source's census of the real passes and textures (the doc facts)
        // stays true. They record nothing: the picture is unchanged.
        static const bool s_uavPair = Sys::CommandLine::Get().Has("-rgtest_uavpair");
        if (s_uavPair && m_snapshotBoard && !m_testUavPairAdded)
        {
            m_testUavPairAdded = true;
            const RGTextureHandle uav = CreateTexture("RGTest.UavPair",
                { RHI::Format::R32_FLOAT, 64, 64, false, true });
            for (const char* name : { "RGTest.UavA", "RGTest.UavB" })
                AddPass(name, [uav](RenderGraphBuilder& b) { b.WriteUAV(uav); },
                        [](RHI::CommandList&) {});
        }

        // ---- -rgtest_unread: the cull report's reverse arm ----
        // A no-op pass at the end of the main graph rendering into a small graph
        // texture nothing reads: [RG_CULL] must list it (candidates=1, its
        // snapshot pass unread). It records nothing - the picture is unchanged.
        static const bool s_unread = Sys::CommandLine::Get().Has("-rgtest_unread");
        if (s_unread && m_snapshotBoard && !m_testUnreadAdded)
        {
            m_testUnreadAdded = true;
            const RGTextureHandle t = CreateTexture("RGTest.Unread", { RHI::Format::R8_UNORM, 8, 8, false, false });
            AddPass("RGTest.Unread", [t](RenderGraphBuilder& b) { b.RenderTarget(t, Load::Load); },
                    [](RHI::CommandList&) {});
        }

        // ---- -rgtest_clearafterlive: ClearAfterLive's reverse arm ----
        // A no-op pass at the END of the main graph declaring the HDR import a
        // Load::Clear render target: the DAG must call it ClearAfterLive (an
        // ERROR under -rgdagverify). Only a declaration - Phase A clears for
        // the HdrSceneColor shim alone - so the picture is unchanged.
        static const bool s_clearAfterLive = Sys::CommandLine::Get().Has("-rgtest_clearafterlive");
        if (s_clearAfterLive && m_snapshotBoard && !m_testClearAfterLiveAdded)
        {
            m_testClearAfterLiveAdded = true;
            AddPass("RGTest.ClearAfterLive",
                    [](RenderGraphBuilder& b) { b.RenderTarget(b.HdrScene(), Load::Clear); },
                    [](RHI::CommandList&) {});
        }
#endif

        // A secondary view's textures are bound only inside its segment, never across a Compile.
        ENGINE_CHECK(m_boundSet == 0, "RenderGraph::Compile while view %u's textures are bound", m_boundSet);
        ++m_compileCount;

        // Free physical textures from any previous compile. An import is the
        // device's, not ours.
        for (auto& vt : m_textures)
            if (vt.texture.IsValid() && !vt.imported) gfx.DestroyTexture(vt.texture);

        m_textures.clear();
        m_nameToIndex.clear();

        // Pre-populate from graph-scope textures (handles from CreateTexture and
        // ImportTexture are stable). An import's texture is the last one bound
        // (or its source's); the next Execute takes this frame's.
        for (auto& gt : m_graphTextures)
        {
            uint32_t idx = static_cast<uint32_t>(m_textures.size());
            VirtualTexture vt;
            vt.name     = gt.name;
            vt.desc     = gt.desc;
            vt.imported = gt.imported;
            vt.outsideDeclared = !gt.outsideBy.empty();
            if (gt.imported)
            {
                vt.source   = gt.spec.source;
                vt.entry    = gt.spec.entry;
                vt.tracking = gt.spec.tracking;
                vt.texture  = gt.spec.source ? *gt.spec.source : gt.bound;
            }
            m_textures.push_back(std::move(vt));
            m_nameToIndex[gt.name] = idx;
        }

#if WITH_HARNESS
        // ---- -rgtest_dupname=<pass>: the rg_instance_names pack ----
        // Append a no-op lambda pass named like an existing pass of the main
        // graph, once, so the instance naming below has a duplicate to resolve.
        if (m_snapshotBoard && !m_testDupNameApplied)
        {
            static const std::string s_dup = Sys::CommandLine::Get().Get("-rgtest_dupname");
            if (!s_dup.empty())
            {
                m_testDupNameApplied = true;
                AddPass(s_dup.c_str(), [](RenderGraphBuilder&) {}, [](RHI::CommandList&) {});
            }
        }

        // ---- -rgtest_ordercycle / -rgtest_registershuffle=<seed>: the first sort's reverse arms ----
        // On the main graph, once, before its first sort.
        //   -rgtest_ordercycle orders the first two passes of the first stage
        //   that has two each After the other: the sort must report the cycle
        //   (ENGINE_CHECK, cycles=1) and keep that stage's registration order,
        //   so the picture is unchanged.
        //   -rgtest_registershuffle deals each stage's registration numbers out
        //   to its passes in a seeded order, as if they had been added so:
        //   every constraint must still hold (the snapshot's `registered` and
        //   `order` show it); the other passes follow the dealt order, so the
        //   picture and the DAG may break - a reverse arm.
        if (m_snapshotBoard && !m_testOrderArmsApplied)
        {
            m_testOrderArmsApplied = true;
            const Sys::CommandLine& cmd = Sys::CommandLine::Get();
            if (cmd.Has("-rgtest_ordercycle"))
            {
                PassNode* a = nullptr;
                PassNode* b = nullptr;
                for (size_t i = 0; i < m_passes.size() && !b; ++i)
                    for (size_t j = i + 1; j < m_passes.size() && !b; ++j)
                        if (m_passes[i].stage < Stage::Count && m_passes[j].stage == m_passes[i].stage)
                        {
                            a = &m_passes[i];
                            b = &m_passes[j];
                        }
                if (ENGINE_CHECK(b != nullptr, "-rgtest_ordercycle: no stage of the main graph has two passes"))
                {
                    a->order.push_back({ Order::Kind::After, b->pass->GetName(), "-rgtest_ordercycle" });
                    b->order.push_back({ Order::Kind::After, a->pass->GetName(), "-rgtest_ordercycle" });
                    m_stagesChecked = false;
                    LOG_INFO("[RG_ORDER] -rgtest_ordercycle: '%s' After '%s' and '%s' After '%s' (stage %s)",
                             a->pass->GetName(), b->pass->GetName(), b->pass->GetName(),
                             a->pass->GetName(), StageName(a->stage));
                }
            }
            if (cmd.Has("-rgtest_registershuffle"))
            {
                const int seed = cmd.GetInt("-rgtest_registershuffle", 1);
                uint32_t state = static_cast<uint32_t>(seed);
                for (size_t k = 0; k < static_cast<size_t>(Stage::Count); ++k)
                {
                    std::vector<PassNode*> nodes;
                    std::vector<uint32_t>  numbers;
                    for (PassNode& node : m_passes)
                        if (node.stage == static_cast<Stage>(k))
                        {
                            nodes.push_back(&node);
                            numbers.push_back(node.registered);
                        }
                    for (size_t i = numbers.size(); i > 1; --i)   // Fisher-Yates on a fixed LCG: every build deals alike
                    {
                        state = state * 1664525u + 1013904223u;
                        std::swap(numbers[i - 1], numbers[(state >> 8) % i]);
                    }
                    for (size_t i = 0; i < nodes.size(); ++i) nodes[i]->registered = numbers[i];
                }
                m_stagesChecked = false;
                LOG_INFO("[RG_ORDER] -rgtest_registershuffle=%d: each stage's registration order dealt out at random",
                         seed);
            }
        }
#endif

        // The first sort: once per pass-list change, before anything reads a
        // position - the instance names below, the stage check, Setup, the DAG.
        const bool passListChanged = !m_stagesChecked;
        if (passListChanged)
        {
            SortPasses();
            m_scheduleApplied = false;   // the second sort runs again after this Setup
        }

        // Instance names. GPU timestamps, PIX markers, DEPTHTRACE labels,
        // command-list names and SetExternalWait all key on the name, so the second
        // pass of one name in this graph is "Name#2", the third "Name#3". Resolved
        // here rather than in AddPass because a pass may take a name suffix after it
        // is added (SetNameSuffix), and after the first sort, which decides which one
        // is the second.
        {
            std::unordered_map<std::string, uint32_t> seen;
            for (PassNode& node : m_passes)
            {
                const char* base = node.pass->GetName();
                const uint32_t k = ++seen[base];
                std::string name = k == 1 ? std::string(base) : std::string(base) + "#" + std::to_string(k);
                if (k > 1)
                    LOG_INFO("[RG] instance name '%s': pass %u of that name in this graph", name.c_str(), k);
                if (node.pass->m_instanceName != name)
                    node.pass->m_instanceName = std::move(name);
            }
            // A generated "Name#2" can still meet a pass that is literally named so.
            std::unordered_map<std::string, size_t> taken;
            for (size_t i = 0; i < m_passes.size(); ++i)
            {
                const auto [it, fresh] = taken.emplace(m_passes[i].pass->InstanceName(), i);
                ENGINE_CHECK_ONCE(fresh,
                    "RenderGraph: passes #%zu and #%zu are both named '%s' - "
                    "timestamps, markers and external waits key on the name",
                    it->second, i, it->first.c_str());
            }
        }

        // Stage order: once per pass-list change, so a resize recompile does not
        // repeat it, and before -rgtest_swap below, which breaks the order on purpose.
        // After the instance names, which it prints. It checks the first sort's
        // result: a registration that went back a stage is [RG_ORDER]'s inverted=,
        // not an error.
        if (passListChanged)
        {
            m_stagesChecked = true;
            CheckStageOrder();
        }

#if WITH_HARNESS
        // ---- -rgtest_swap=<a>,<b>: the rg_dag pack's reverse arm ----
        // Swap two passes' declaration order - which IS the execution order - in the
        // main graph, once, before the first Setup. A consumer moved in front of its
        // producer reads what nobody wrote this frame, and the DAG's StaleRead check
        // must say so. The picture is wrong on purpose; the barriers stay valid
        // (Phase A transitions from the tracker).
        if (m_snapshotBoard && !m_testSwapApplied)
        {
            static const std::string s_swap = Sys::CommandLine::Get().Get("-rgtest_swap");
            if (!s_swap.empty())
            {
                m_testSwapApplied = true;
                const size_t comma = s_swap.find(',');
                const std::string a = s_swap.substr(0, comma);
                const std::string b = comma == std::string::npos ? std::string() : s_swap.substr(comma + 1);
                size_t ia = m_passes.size(), ib = m_passes.size();
                for (size_t i = 0; i < m_passes.size(); ++i)
                {
                    if (a == m_passes[i].pass->InstanceName()) ia = i;
                    if (b == m_passes[i].pass->InstanceName()) ib = i;
                }
                if (ENGINE_CHECK(ia < m_passes.size() && ib < m_passes.size() && ia != ib,
                                 "-rgtest_swap=%s: needs two different pass names of the main graph",
                                 s_swap.c_str()))
                {
                    std::swap(m_passes[ia], m_passes[ib]);
                    LOG_INFO("[RG_DAG] -rgtest_swap: '%s' (#%zu) <-> '%s' (#%zu)",
                             a.c_str(), ia, b.c_str(), ib);
                }
            }
        }
#endif

        // Every pass names the textures it hands over BEFORE any Setup runs, so a
        // reader declared earlier than its producer finds the name
        // (RenderPass::DeclareImports). -rgimportoff: no imports at all.
        if (ImportsEnabled())
            for (auto& node : m_passes)
            {
                ImportDeclarer declarer(*this);
                node.pass->DeclareImports(declarer);
            }

        // Run Setup on each pass to collect read/write declarations.
        for (auto& node : m_passes)
        {
            node.rtHandles.clear();
            node.dsHandles.clear();
            node.srvHandles.clear();
            node.historyHandles.clear();
            node.uavHandles.clear();
            // The manual lists are re-declared by Setup like the others, so a pass
            // whose Setup stops declaring one does not keep the stale entry (and its
            // entry state).
            node.manualHandles.clear();
            node.manualEntryStates.clear();
            node.rtLoads.clear();
            node.scopedHandles.clear();
            node.scopedStates.clear();
            node.dsvDecls.clear();
            node.stencilSrvHandles.clear();
            node.copySrc.clear();
            node.copyDst.clear();
            node.depthViews.clear();
            node.hdrShim     = {};
            node.colorTarget = BuiltinTexture::None;

            m_currentSetupNode = &node;
            RenderGraphBuilder builder(*this);
            node.pass->Setup(builder);
            m_currentSetupNode = nullptr;
        }

        // Stage rule (RenderGraph/Stage.h): only the GBuffer stage clears the HDR
        // scene colour. HdrSceneColor is bind + CLEAR, the first HDR writer's
        // declaration; a later stage declaring it would wipe what the stages before it
        // drew (HdrSceneColorPreserve binds without the clear). Every Compile, since
        // Setup declares it; staged passes only - a preview graph's GBufferPass is its
        // own first writer. With the HDR scene colour imported the shim declares a
        // Load::Clear render target and the DAG's ClearAfterLive judges it (in any
        // stage, on the frames the passes run), so this rule covers only the graph
        // without an import (-rgimportoff).
        for (const PassNode& node : m_passes)
            if (node.colorTarget == BuiltinTexture::HdrSceneColor && node.stage != Stage::Unstaged &&
                !node.hdrShim.IsValid())
                ENGINE_CHECK_ONCE(node.stage == Stage::GBuffer,
                    "RenderGraph: pass '%s' (stage %s) declares HdrSceneColor, which clears the HDR scene colour - "
                    "only the GBuffer stage may; a later writer binds HdrSceneColorPreserve",
                    node.pass->InstanceName(), StageName(node.stage));

        // Propagate per-pass UAV/SRV declarations into the per-texture flags so
        // the physical creation step picks up bind flags for textures written
        // as UAV from any pass (e.g. in-place GBuffer decal compositing).
        for (const auto& node : m_passes)
        {
            for (const auto& h : node.uavHandles)
                if (h.IsValid() && h.id < m_textures.size())
                    m_textures[h.id].hasUAV = true;
            for (const auto& h : node.srvHandles)
                if (h.IsValid() && h.id < m_textures.size())
                    m_textures[h.id].hasSRV = true;
        }
        // Also honour isUAV / isSRV declared at CreateTexture time. isSRV is for
        // textures read as an SRV from OUTSIDE the graph (e.g. GBuffer velocity read by
        // TAA / XeGTAO / SSR through direct SRV handles): no in-graph pass declares the
        // read, so without it the resource is created without SHADER_RESOURCE and
        // GetTextureSRVGpuHandle returns 0.
        for (auto& vt : m_textures)
        {
            if (vt.desc.isUAV) vt.hasUAV = true;
            if (vt.desc.isSRV) vt.hasSRV = true;
        }

        // Each pass's declarations as one list, in the order Phase A transitions
        // them (render targets, depth, UAVs, reads, the scoped reads, then the manual
        // handles) - what the per-frame DAG and the snapshot read. Pass and texture
        // indices may have changed, so the logged findings start over.
        m_passAccesses.assign(m_passes.size(), {});
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            const PassNode& n = m_passes[i];
            auto& acc = m_passAccesses[i];
            auto add = [&](const std::vector<RGTextureHandle>& hs, GraphSnapshot::Access kind)
            {
                for (RGTextureHandle h : hs)
                    if (h.IsValid() && h.id < m_textures.size())
                        acc.push_back({ h.id, kind, 0u });
            };
            // Render targets carry their load op.
            for (size_t k = 0; k < n.rtHandles.size(); ++k)
            {
                const RGTextureHandle h = n.rtHandles[k];
                if (!h.IsValid() || h.id >= m_textures.size()) continue;
                GraphSnapshot::PassAccess a{ h.id, GraphSnapshot::Access::RenderTarget, 0u };
                a.load = static_cast<uint8_t>(k < n.rtLoads.size() ? n.rtLoads[k] : Load::Load);
                acc.push_back(a);
            }
            add(n.dsHandles,  GraphSnapshot::Access::Depth);
            add(n.uavHandles, GraphSnapshot::Access::UAV);
            // A DepthStencil declaration per plane - a write, a test (DsvRead), or
            // nothing; then the copies. Under -rgplaneoff, the whole texture written (as
            // WriteDepthStencil) and whole copies.
            const bool planes = PlanesEnabled();
            for (const PassNode::DsvDecl& d : n.dsvDecls)
            {
                if (!d.h.IsValid() || d.h.id >= m_textures.size()) continue;
                const bool planar = m_textures[d.h.id].desc.isDepth && RHI::HasStencilPlane(m_textures[d.h.id].desc.format);
                if (!planes)
                {
                    acc.push_back({ d.h.id, GraphSnapshot::Access::Depth, 0u });
                    continue;
                }
                auto plane = [&](bool write, bool read, int8_t p, uint8_t mask)
                {
                    if (!write && !read) return;
                    GraphSnapshot::PassAccess a{ d.h.id, write ? GraphSnapshot::Access::Depth
                                                               : GraphSnapshot::Access::DsvRead, 0u };
                    a.plane = planar ? p : -1;
                    a.stencilMask = mask;
                    acc.push_back(a);
                };
                plane(d.depth == Depth::Write, d.depth == Depth::Read, 0, 0xFF);
                if (planar)
                    plane(d.stencil == Stencil::Write, d.stencil == Stencil::Read, 1, d.stencilMask);
            }
            for (bool dst : { true, false })
                for (const auto& c : dst ? n.copyDst : n.copySrc)
                {
                    if (!c.first.IsValid() || c.first.id >= m_textures.size()) continue;
                    const VirtualTexture& vt = m_textures[c.first.id];
                    GraphSnapshot::PassAccess a{ c.first.id, dst ? GraphSnapshot::Access::CopyDst
                                                                 : GraphSnapshot::Access::CopySrc, 0u };
                    if (planes && vt.desc.isDepth && RHI::HasStencilPlane(vt.desc.format))
                        a.plane = static_cast<int8_t>(c.second);
                    acc.push_back(a);
                }
            for (RGTextureHandle h : n.srvHandles)
            {
                if (!h.IsValid() || h.id >= m_textures.size()) continue;
                const bool history = std::any_of(n.historyHandles.begin(), n.historyHandles.end(),
                                                 [h](RGTextureHandle x) { return x.id == h.id; });
                GraphSnapshot::PassAccess a{ h.id, history ? GraphSnapshot::Access::History
                                                           : GraphSnapshot::Access::Read, 0u };
                // A depth-stencil texture's SRV is its depth plane's.
                if (planes && m_textures[h.id].desc.isDepth && RHI::HasStencilPlane(m_textures[h.id].desc.format))
                    a.plane = 0;
                acc.push_back(a);
            }
            for (RGTextureHandle h : n.stencilSrvHandles)   // the stencil plane's SRV
            {
                if (!h.IsValid() || h.id >= m_textures.size()) continue;
                GraphSnapshot::PassAccess a{ h.id, GraphSnapshot::Access::Read, 0u };
                if (planes && RHI::HasStencilPlane(m_textures[h.id].desc.format)) a.plane = 1;
                acc.push_back(a);
            }
            for (size_t k = 0; k < n.scopedHandles.size(); ++k)
            {
                const RGTextureHandle h = n.scopedHandles[k];
                if (!h.IsValid() || h.id >= m_textures.size()) continue;
                const uint32_t state = k < n.scopedStates.size()
                                     ? static_cast<uint32_t>(n.scopedStates[k]) : 0u;
                acc.push_back({ h.id, GraphSnapshot::Access::Scoped, state });
            }
            for (size_t k = 0; k < n.manualHandles.size(); ++k)
            {
                const RGTextureHandle h = n.manualHandles[k];
                if (!h.IsValid() || h.id >= m_textures.size()) continue;
                const uint32_t entry = k < n.manualEntryStates.size()
                                     ? static_cast<uint32_t>(n.manualEntryStates[k]) : 0u;
                acc.push_back({ h.id, GraphSnapshot::Access::Manual, entry });
            }
        }
        m_dagReported.clear();
        // The depth-stencil formats - two planes, a state each (not under
        // -rgplaneoff, which tracks them whole).
        m_planar.assign(m_textures.size(), 0);
        for (size_t t = 0; t < m_textures.size(); ++t)
        {
            VirtualTexture& vt = m_textures[t];
            vt.planar   = vt.desc.isDepth && RHI::HasStencilPlane(vt.desc.format);
            m_planar[t] = vt.planar && PlanesEnabled() ? 1 : 0;
        }
        // Each pass's stencil bits against the registry.
        if (PlanesEnabled()) CheckStencilOwners();
        // The imports written outside, before the graph (Fresh).
        m_importFresh.assign(m_textures.size(), 0);
        for (size_t t = 0; t < m_textures.size(); ++t)
            m_importFresh[t] = m_textures[t].imported && m_textures[t].entry == ImportEntry::Fresh ? 1 : 0;
        // The textures a secondary view shares with the main view: no pass that records in it
        // (kViewSafe) writes them, so their content is what the main view's passes made this frame.
        m_viewShared.assign(m_textures.size(), 1);
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            if (!m_passes[i].pass || !(m_passes[i].pass->HandledEvents() & PassLifecycle::kViewSafe)) continue;
            for (const GraphSnapshot::PassAccess& a : m_passAccesses[i])
            {
                using A = GraphSnapshot::Access;
                if (a.kind == A::RenderTarget || a.kind == A::Depth || a.kind == A::UAV || a.kind == A::Manual ||
                    a.kind == A::CopyDst)
                    m_viewShared[a.texture] = 0;
            }
        }
        for (size_t t = 0; t < m_textures.size(); ++t)
            if (m_textures[t].imported) m_viewShared[t] = 0;   // bound per Execute

        // The second sort (-rgschedule): once per pass-list change, with the
        // declarations this Setup made; a recompile keeps the order it chose (Setup
        // runs in it).
        if (!m_scheduleApplied)
        {
            m_scheduleApplied = true;
            ApplySchedule();
        }

        CreatePhysicalTextures(gfx);
        BuildAliasPlan(gfx);

        // Per-pass setup time (the renderer's share of boot): Init loads a pass's
        // shaders and builds its PSOs, OnCompile its size-dependent resources. The
        // slowest few are logged when a compile takes over 200 ms.
        using SetupClock = std::chrono::steady_clock;
        std::vector<double> setupMs(m_passes.size(), 0.0);
        const auto setupStart = SetupClock::now();
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            auto& node = m_passes[i];
            if (!node.initialized)
            {
                const auto t = SetupClock::now();
                node.pass->Init(gfx);
                setupMs[i] += std::chrono::duration<double, std::milli>(SetupClock::now() - t).count();
                node.initialized = true;
            }
        }

        // Give every pass a chance to (re)create its own GPU resources in the serial
        // phase - must happen BEFORE the parallel Execute phase to avoid data races on
        // the descriptor heap allocator.
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            const auto t = SetupClock::now();
            m_passes[i].pass->OnCompile(gfx);
            setupMs[i] += std::chrono::duration<double, std::milli>(SetupClock::now() - t).count();
        }
        const double setupTotal =
            std::chrono::duration<double, std::milli>(SetupClock::now() - setupStart).count();
        if (setupTotal > 200.0)
        {
            std::vector<size_t> order(m_passes.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return setupMs[a] > setupMs[b]; });
            std::string slowest;
            for (size_t k = 0; k < order.size() && k < 8; ++k)
            {
                char one[96];
                std::snprintf(one, sizeof(one), "%s%s %.0f", k ? ", " : "",
                              m_passes[order[k]].pass->InstanceName(), setupMs[order[k]]);
                slowest += one;
            }
            LOG_INFO("[RGSETUP] %zu passes set up in %.0f ms (Init + OnCompile); slowest (ms): %s",
                     m_passes.size(), setupTotal, slowest.c_str());
        }

        m_compiled = true;
        uint32_t onDemand = 0, onDemandBacked = 0;
        for (size_t i = 0; i < m_graphTextures.size(); ++i)
            if (m_graphTextures[i].residency == TextureResidency::OnDemand)
            {
                ++onDemand;
                if (i < m_textures.size() && m_textures[i].texture.IsValid()) ++onDemandBacked;
            }
        if (onDemand)
            LOG_INFO("RenderGraph: compiled %zu passes, %zu virtual textures "
                     "(%u on demand, %u of them backed)",
                     m_passes.size(), m_textures.size(), onDemand, onDemandBacked);
        else
            LOG_INFO("RenderGraph: compiled %zu passes, %zu virtual textures",
                     m_passes.size(), m_textures.size());
    }

    // ---- Teardown ----
    // See the header for the contract. The order is load-bearing in one place:
    // every pass's Shutdown runs BEFORE any physical texture is destroyed, because
    // a pass may hold a descriptor or an SRV that names one of them and its own
    // bookkeeping must be what gives it back.
    RenderGraph::DestroyStats RenderGraph::Destroy(IGraphicsDevice& gfx)
    {
        DestroyStats st;
        st.passes = static_cast<uint32_t>(m_passes.size());
        for (auto& node : m_passes)
            if (node.pass) node.pass->Shutdown(gfx);

        for (size_t i = 0; i < m_textures.size(); ++i)
        {
            VirtualTexture& vt = m_textures[i];
            // An import is the device's; a shared texture under a bound secondary view is slot 0's,
            // destroyed with its set below.
            if (vt.texture.IsValid() && !vt.imported && !(m_boundSet != 0 && ViewShared(i)))
            {
                gfx.DestroyTexture(vt.texture);   // deferred bucket
                ++st.textures;
            }
        }
        // The views' sets that are not bound (slot 0's when a secondary one is).
        for (uint32_t s = 0; s < RHI::kMaxViews; ++s)
        {
            if (s != m_boundSet)
                for (ViewTexture& v : m_viewSets[s].textures)
                    if (v.texture.IsValid())
                    {
                        gfx.DestroyTexture(v.texture);
                        ++st.textures;
                    }
            m_viewSets[s] = ViewSet{};
        }
        m_boundSet = 0;
        m_viewSkipLogged.clear();

        m_passes.clear();
        m_textures.clear();
        m_viewShared.clear();
        m_graphTextures.clear();
        m_nameToIndex.clear();
        {
            std::lock_guard<std::mutex> lock(m_aliasMutex);
            m_aliasPlan.clear();
        }
        m_auditSeen.reset();
        m_auditSeenCount = 0;
        m_view = SceneView{};   // a new tenancy shows everything until its owner says otherwise
        m_externalWaits.clear();
        m_cbSlotBindings.fill(nullptr);
        m_bufferSlotBindings.fill(nullptr);
        m_cbFallback.clear();
        m_bufferFallback.clear();
        m_drawList            = nullptr;
        m_bindlessTableHandle = 0;
        // Pinned dimensions go too: a rebuilt graph is a NEW tenancy and its
        // caller sets the size it wants before Compile. Leaving the old pair
        // behind would silently size a rebuild at the size the previous panel
        // happened to close at.
        m_fixedWidth = m_fixedHeight = 0;
        m_compiled            = false;
        m_registered          = 0;   // a new tenancy numbers its AddPasses from 0
        m_currentSetupNode    = nullptr;
        m_extWaitRehomeLogged = false;
        m_extWaitDroppedLogged = false;
#if RG_VALIDATE_ACCESS
        // Indexed BY PASS, and there are no passes any more. Keeping them would
        // attribute the next tenancy's accesses to the previous one's slots.
        m_undeclaredByPass.clear();
        m_reportedByPass.clear();
        m_validationSummaryLogged = false;
#endif
        return st;
    }

    void RenderGraph::CreatePhysicalTextures(IGraphicsDevice& gfx)
    {
        // A preview graph pins its own size; 0 = ask the device (the default).
        m_physicalWidth  = m_fixedWidth  ? m_fixedWidth  : gfx.GetRenderWidth();
        m_physicalHeight = m_fixedHeight ? m_fixedHeight : gfx.GetRenderHeight();

        for (size_t i = 0; i < m_textures.size(); ++i)
        {
            // An OnDemand texture nobody has asked for stays a declaration:
            // no memory, no descriptor (see TextureResidency).
            if (i < m_graphTextures.size() &&
                m_graphTextures[i].residency == TextureResidency::OnDemand &&
                !m_graphTextures[i].demanded)
                continue;
            // An import is never created here: BindImport hands it over.
            if (m_textures[i].imported) continue;
            CreatePhysicalTexture(m_textures[i], gfx);
        }
    }

    bool RenderGraph::CreatePhysicalTexture(VirtualTexture& vt, IGraphicsDevice& gfx)
    {
        if (vt.texture.IsValid()) return true;

        RHI::ResourceState layout = RHI::ResourceState::UNDEFINED;
        const RHI::TextureDesc tdesc = PhysicalDesc(vt, layout);
        vt.currentState = layout;
        if (vt.desc.isDepth) vt.stencilState = layout;   // the stencil plane too

        if (!gfx.CreateTexture(tdesc, vt.texture))
        {
            LOG_ERROR("RenderGraph: CreateTexture failed for '%s'", vt.name.c_str());
            return false;
        }
        return true;
    }

    RHI::TextureDesc RenderGraph::PhysicalDesc(const VirtualTexture& vt, RHI::ResourceState& layout) const
    {
        RHI::TextureDesc tdesc;
        tdesc.format = vt.desc.format;
        tdesc.width  = vt.desc.width  ? vt.desc.width  : m_physicalWidth;
        tdesc.height = vt.desc.height ? vt.desc.height : m_physicalHeight;
        tdesc.usage  = RHI::Usage::DEFAULT;
        tdesc.arraySize = vt.desc.arraySize;

        RHI::BindFlag flags = RHI::BindFlag::NONE;
        if (vt.hasRTV) flags |= RHI::BindFlag::RENDER_TARGET;
        // isDepth textures need the DS flag even when no pass declares a
        // depth WRITE (e.g. SSRTraceDepth is only ever copy-dst + SRV):
        // they are created in DEPTHSTENCIL layout below, which D3D12
        // rejects without ALLOW_DEPTH_STENCIL on the resource.
        if (vt.hasDSV || vt.desc.isDepth) flags |= RHI::BindFlag::DEPTH_STENCIL;
        if (vt.hasSRV) flags |= RHI::BindFlag::SHADER_RESOURCE;
        if (vt.hasUAV) flags |= RHI::BindFlag::UNORDERED_ACCESS;
        tdesc.bindFlags = flags;

        if (vt.desc.isDepth)
        {
            tdesc.layout = RHI::ResourceState::DEPTHSTENCIL;
            tdesc.clear  = RHI::ClearValue::DepthStencil(0.0f, 0);  // reversed Z: far = 0
        }
        else if (vt.hasRTV)
        {
            tdesc.layout = RHI::ResourceState::RENDERTARGET;
            tdesc.clear  = RHI::ClearValue::Color(0, 0, 0, 1);
        }
        else
        {
            // Compute-only outputs have no ALLOW_RENDER_TARGET flag.
            // Their creation state must match their actual bindings.
            tdesc.layout = vt.hasUAV ? RHI::ResourceState::UNORDERED_ACCESS
                                     : RHI::ResourceState::SHADER_RESOURCE;
        }
        layout = tdesc.layout;

        // Forward the virtual-texture name to the resource so D3D12
        // validation messages identify it instead of "Unnamed".
        tdesc.debugName = vt.name.c_str();
        return tdesc;
    }

    RenderGraph::~RenderGraph() = default;

    // ---- Memory aliasing ----

    const char* RenderGraph::AliasPinName(AliasPin pin)
    {
        switch (pin)
        {
        case AliasPin::None:          return "candidate";
        case AliasPin::Imported:      return "imported";
        case AliasPin::OnDemand:      return "onDemand";
        case AliasPin::OutsideUse:    return "outsideUse";
        case AliasPin::FirstUseLoads: return "firstUseLoads";
        case AliasPin::CrossFrame:    return "crossFrame";
        case AliasPin::Unused:        return "unused";
        }
        return "?";
    }

    void RenderGraph::DeclareOutsideUse(RGTextureHandle h, const char* who)
    {
        if (!h.IsValid() || h.id >= m_graphTextures.size() || !who || !*who) return;
        std::string& by = m_graphTextures[h.id].outsideBy;
        if (by.find(who) != std::string::npos) return;   // already named (a second Compile of the same tenancy)
        if (!by.empty()) by += ", ";
        by += who;
    }

    std::vector<RenderGraph::AliasRow> RenderGraph::AliasPlan() const
    {
        std::lock_guard<std::mutex> lock(m_aliasMutex);
        return m_aliasPlan;
    }

    void RenderGraph::AuditPhysicalAccess(uint32_t id) const
    {
#if RG_VALIDATE_ACCESS
        if (!m_auditSeen || id >= m_auditSeenCount || id >= m_textures.size()) return;
        const VirtualTexture& vt = m_textures[id];
        if (vt.imported) return;
        const int pi  = s_tlsCurrentGraph == this ? s_tlsCurrentPass : -1;
        uint8_t   bit = 1;
        if (pi >= 0 && pi < static_cast<int>(m_passAccesses.size()))
        {
            for (const GraphSnapshot::PassAccess& a : m_passAccesses[pi])
                if (a.texture == id) return;
            bit = 2;
        }
        else if (vt.outsideDeclared)
            return;
        if (m_auditSeen[id].fetch_or(bit, std::memory_order_relaxed) & bit) return;
        if (bit == 1)
            LOG_INFO("[RG_ALIAS] audit: '%s' is used outside the graph's passes without DeclareOutsideUse",
                     vt.name.c_str());
        else
            LOG_INFO("[RG_ALIAS] audit: pass '%s' takes '%s' (GetPhysicalTexture) without declaring it",
                     m_passes[pi].pass->InstanceName(), vt.name.c_str());
#else
        (void)id;
#endif
    }

    void RenderGraph::BuildAliasPlan(IGraphicsDevice& gfx)
    {
        using Access = GraphSnapshot::Access;
        const size_t n = m_textures.size();
        std::vector<AliasRow> rows(n);
        std::vector<uint64_t> align(n, 0);
        for (size_t t = 0; t < n; ++t) rows[t].name = m_textures[t].name;

        // Lifetimes: the first and the last pass that declares each texture, in execution order,
        // active or not - a placement must hold for every frame of this Compile.
        for (size_t i = 0; i < m_passAccesses.size(); ++i)
            for (const GraphSnapshot::PassAccess& a : m_passAccesses[i])
            {
                if (a.texture >= n) continue;
                AliasRow& r = rows[a.texture];
                if (r.first < 0) r.first = static_cast<int32_t>(i);
                r.last = static_cast<int32_t>(i);
            }

        for (size_t t = 0; t < n; ++t)
        {
            const VirtualTexture& vt = m_textures[t];
            const GraphTexture*   gt = t < m_graphTextures.size() ? &m_graphTextures[t] : nullptr;
            AliasRow&             r  = rows[t];
            if (gt) r.outsideBy = gt->outsideBy;
            r.rtds = vt.hasRTV || vt.hasDSV || vt.desc.isDepth;
            if (vt.imported)
            {
                r.pin = AliasPin::Imported;
                continue;
            }
            if (gt && gt->residency == TextureResidency::OnDemand) r.pin = AliasPin::OnDemand;
            else if (!r.outsideBy.empty())                         r.pin = AliasPin::OutsideUse;
            else if (r.first < 0)                                  r.pin = AliasPin::Unused;
            else
            {
                // What its first pass does to it: a read sees last frame's bytes, and so does a write
                // that keeps what is there (a Load render target, a DSV, a UAV, a self-barriered
                // access, one plane's copy). Only a Clear / DontCare render target or a whole copy
                // starts it afresh.
                bool reads = false, keeps = false;
                for (const GraphSnapshot::PassAccess& a : m_passAccesses[r.first])
                {
                    if (a.texture != t) continue;
                    switch (a.kind)
                    {
                    case Access::Read: case Access::History: case Access::Scoped:
                    case Access::DsvRead: case Access::CopySrc:
                        reads = true;
                        break;
                    case Access::RenderTarget:
                        if (a.load == static_cast<uint8_t>(Load::Load)) keeps = true;
                        break;
                    case Access::CopyDst:
                        if (vt.planar && a.plane >= 0) keeps = true;
                        break;
                    case Access::Depth: case Access::UAV: case Access::Manual:
                        keeps = true;
                        break;
                    }
                }
                r.pin = reads ? AliasPin::CrossFrame : keeps ? AliasPin::FirstUseLoads : AliasPin::None;
            }
            RHI::ResourceState layout = RHI::ResourceState::UNDEFINED;
            uint64_t size = 0, al = 0;
            if (gfx.GetTextureFootprint(PhysicalDesc(vt, layout), size, al))
            {
                r.bytes  = size;
                align[t] = al;
            }
        }

        // What the candidates would take packed: first fit by offset, largest first, two textures
        // sharing bytes only where their lifetimes do not overlap.
        auto pack = [&](bool rtds) -> uint64_t
        {
            std::vector<size_t> ids;
            for (size_t t = 0; t < n; ++t)
                if (rows[t].pin == AliasPin::None && rows[t].rtds == rtds && rows[t].bytes) ids.push_back(t);
            std::sort(ids.begin(), ids.end(), [&](size_t a, size_t b) { return rows[a].bytes > rows[b].bytes; });
            struct Block
            {
                uint64_t off, end;
                int32_t  first, last;
            };
            std::vector<Block> placed;
            uint64_t heap = 0;
            for (size_t t : ids)
            {
                const AliasRow& r = rows[t];
                const uint64_t  a = std::max<uint64_t>(align[t], 65536);
                uint64_t off = 0;
                for (bool moved = true; moved;)
                {
                    moved = false;
                    for (const Block& b : placed)
                        if (r.first <= b.last && b.first <= r.last && off < b.end && b.off < off + r.bytes)
                        {
                            off   = (b.end + a - 1) / a * a;
                            moved = true;
                        }
                }
                placed.push_back({ off, off + r.bytes, r.first, r.last });
                heap = std::max(heap, off + r.bytes);
            }
            return heap;
        };
        const uint64_t heapRtDs  = pack(true);
        const uint64_t heapOther = pack(false);

        if (m_snapshotBoard)
        {
            uint32_t count[7] = {};
            uint64_t created = 0, candidates = 0;
            for (size_t t = 0; t < n; ++t)
            {
                ++count[static_cast<size_t>(rows[t].pin)];
                if (!m_textures[t].imported && m_textures[t].texture.IsValid()) created += rows[t].bytes;
                if (rows[t].pin == AliasPin::None) candidates += rows[t].bytes;
            }
            constexpr double kMB = 1.0 / (1024.0 * 1024.0);
            LOG_INFO("[RG_ALIAS] textures=%zu candidates=%u (%.1f MB) pinned: imported=%u onDemand=%u outsideUse=%u "
                     "firstUseLoads=%u crossFrame=%u unused=%u; graph-created %.1f MB, candidates packed rtds=%.1f MB "
                     "other=%.1f MB, would save %.1f MB",
                     n, count[0], candidates * kMB, count[1], count[2], count[3], count[4], count[5], count[6],
                     created * kMB, heapRtDs * kMB, heapOther * kMB,
                     (static_cast<double>(candidates) - static_cast<double>(heapRtDs + heapOther)) * kMB);
        }

        if (m_auditSeenCount != n)
        {
            m_auditSeen      = std::make_unique<std::atomic<uint8_t>[]>(n);
            m_auditSeenCount = n;
        }
        std::lock_guard<std::mutex> lock(m_aliasMutex);
        m_aliasPlan = std::move(rows);
    }

    // ---- Execute ----

    RHI::CommandList RenderGraph::Execute(IGraphicsDevice& gfx, const float clearColor[4])
    {
        Sys::OwnerTaskQueue owner;
        RHI::CommandList result{};
        owner.RunUntil(ExecuteAsync(gfx, clearColor, result).Start(owner, {}, "RenderGraph::Execute"));
        return result;
    }

    Sys::OwnerTask RenderGraph::ExecuteAsync(IGraphicsDevice& gfx, const float* clearColor, RHI::CommandList& result)
    {
        const int profileEvery = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
        std::vector<double> passMs(profileEvery > 0 ? m_passes.size() : 0);
        bool graphRecording = true;
#if WITH_HARNESS
        static const bool s_legacyRecording = Sys::CommandLine::Get().Has("-rgrecordlegacy");
        static const int s_recordAB = Sys::CommandLine::Get().GetInt("-rgrecordab", 0);
        graphRecording = !s_legacyRecording && (!s_recordAB || ((++m_recordScheduleEpoch + s_recordAB) % 2 == 0));
#endif

        if (!m_compiled) Compile(gfx);
        const RHI::CommandList executeAfter = std::exchange(m_executeAfter, RHI::CommandList{});

        const size_t passCount = m_passes.size();
        if (passCount == 0) co_return;

        // ---- Imports: this frame's texture ----
        // Serial, before the snapshot copies the tracker. The tracker starts from the
        // DEVICE's book: whatever touched the texture outside the graph since its last
        // Execute is where this one starts. The bind is taken here, so the next
        // Execute needs its own. An import with a source takes the source's texture as
        // it is now (a pass's own member: a recreation reaches this Execute with no
        // bind), and an Owner-tracked one is only declared - its owner transitions it,
        // and its book may be changing right now (the CSM on worker 0), so it is never
        // read.
        for (size_t i = 0; i < m_textures.size(); ++i)
        {
            VirtualTexture& vt = m_textures[i];
            if (!vt.imported) continue;
            if (vt.source)
                vt.texture = *vt.source;
            else if (i < m_graphTextures.size())
            {
                GraphTexture& gt = m_graphTextures[i];
                ENGINE_CHECK_ONCE(gt.boundThisFrame,
                    "RenderGraph: import '%s' was not bound (BindImport) before this Execute - "
                    "its passes get no texture", gt.name.c_str());
                vt.texture        = gt.boundThisFrame ? gt.bound : RHI::Texture{};
                gt.boundThisFrame = false;
            }
            if (vt.tracking == ImportTracking::Owner)
            {
                vt.currentState = RHI::ResourceState::UNDEFINED;
                continue;
            }
            vt.currentState = vt.texture.IsValid() ? gfx.QueryTextureState(vt.texture, vt.planar ? 0 : -1)
                                                   : RHI::ResourceState::UNDEFINED;
            if (vt.planar)   // a depth-stencil import's stencil plane
                vt.stencilState = vt.texture.IsValid() ? gfx.QueryTextureState(vt.texture, 1)
                                                       : RHI::ResourceState::UNDEFINED;
            ENGINE_CHECK_ONCE(!vt.texture.IsValid() || vt.currentState != RHI::ResourceState::UNDEFINED,
                "RenderGraph: import '%s' enters in a device state no single RHI state names - "
                "the graph cannot transition it", vt.name.c_str());
        }

        // ---- A snapshot of THIS Execute, only when one was asked ----
        // ConsumeRequest is one relaxed load on every frame nobody asked about. The
        // capture scratch lives in this coroutine frame and stays empty unless asked;
        // the tracker's states are copied before Phase A moves them, so a reader can
        // replay every barrier from the start. A frame the device is journaling (a
        // frame capture, RenderGraph/FrameCapture.h) is snapshotted too, and the
        // snapshot is kept for the Renderer to join with the device's.
        // The main view's set only: a snapshot and the frame capture describe the main view.
        const bool journaled = m_snapshotBoard && m_boundSet == 0 && gfx.FrameJournalActive();
        const bool snapshot  = m_snapshotBoard && m_boundSet == 0 &&
                               (m_snapshotBoard->ConsumeRequest() || journaled);
        std::vector<std::vector<GraphSnapshot::Barrier>> snapBarriers;
        std::vector<std::vector<uint32_t>> snapUavBarriers;   // the UAV-after-UAV barriers per pass
        std::vector<uint32_t> snapInitial, snapInitialStencil;   // the tracker's initial states, and the stencil planes'
        if (snapshot)
        {
            snapBarriers.resize(passCount);
            snapUavBarriers.resize(passCount);
            snapInitial.reserve(m_textures.size());
            snapInitialStencil.reserve(m_textures.size());
            for (const VirtualTexture& vt : m_textures)
            {
                snapInitial.push_back(static_cast<uint32_t>(vt.currentState));
                snapInitialStencil.push_back(static_cast<uint32_t>(vt.stencilState));
            }
        }

        // Build the shared read-only context (textures, CBs, draw list). Passes read
        // it concurrently in Phase B - no writes occur after setup.
        if (!ENGINE_CHECK(!m_executing, "RenderGraph::Execute is not reentrant")) co_return;
        m_executing = true;
        struct ExecutionScope
        {
            bool& executing;
            RenderContext& context;
            ~ExecutionScope() { context.Clear(); executing = false; }
        } executionScope{ m_executing, m_executeContext };
        RenderContext& ctx = m_executeContext;
        ctx.Clear();
        // Same source of truth as CreatePhysicalTextures, so a pinned graph hands
        // passes the dimensions its physical textures were built at.
        ctx.SetDimensions(m_fixedWidth  ? m_fixedWidth  : gfx.GetRenderWidth(),
                          m_fixedHeight ? m_fixedHeight : gfx.GetRenderHeight());
        ctx.SetDrawList(m_drawList);
        ctx.SetView(&m_view);   // RenderContext::GetView()

        // Slot bindings were resolved at registration: 2 x Count pointer copies, no
        // string work. The fallback maps are normally empty.
        for (size_t i = 0; i < size_t(RGSlot::Count); ++i)
        {
            if (m_cbSlotBindings[i])     ctx.SetCB(RGSlot(i), *m_cbSlotBindings[i]);
            if (m_bufferSlotBindings[i]) ctx.SetBuffer(RGSlot(i), *m_bufferSlotBindings[i]);
        }

        for (auto& [name, buf] : m_cbFallback)
            if (buf) ctx.SetCB(name.c_str(), *buf);

        for (auto& [name, buf] : m_bufferFallback)
            if (buf) ctx.SetBuffer(name.c_str(), *buf);

        ctx.SetBindlessTableHandle(m_bindlessTableHandle);

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_textures.size()); ++i)
            ctx.SetTexture({ i }, m_textures[i].texture);

        // ---- Two-phase recording: setup serial, Execute parallel ----
        // Phase A (serial, main thread):
        //   Open each pass's CL, set its render target, emit resource barriers, wire
        //   the GPU dependency chain, and open the GPU profiler region. Barriers
        //   cannot run in parallel because they share the texture state tracker
        //   (m_textures[].currentState) - two passes racing on the same texture would
        //   miss a transition.
        // Phase B (parallel, worker threads):
        //   Each pass's work item calls m_passes[i].pass->Execute(passCLs[i]). The
        //   shared RenderContext is read-only per the class contract, and each pass
        //   writes only to its own CommandList. A split pass (GBufferPass,
        //   TransparentPass) adds work items of its own (PrepareRecording) that run
        //   ExecuteAdditionalRecording.
        // Phase C (serial): close profiler regions and order the passes' aux lists
        //   (sequential CL writes after Execute finished).
        auto& recs = m_passRecords;
        recs.assign(passCount, PassRecord{});
        auto& extraTasks = m_extraRecordTasks;
        extraTasks.clear();
        RHI::CommandList prevCL = executeAfter;

        // ---- Pass activity gate (serial, before anything is allocated) ----
        // Queried exactly ONCE per frame, here, so Phases A/B/C cannot disagree. Many
        // of the ~50 passes return on their first line every frame (no water body, no
        // terrain tile, no video, clouds disabled ...), and a command list, a target
        // bind, barriers and an empty profiler region + PIX marker around nothing are
        // pure CPU cost.
        //
        // Skipping is safe: EmitBarriersBeforePass transitions from vt.currentState
        // (STATE-based, not ORDER-based), and a skipped pass touches no texture and
        // mutates no state, so the next active pass emits exactly the transitions it
        // needs.
        //
        // The view ANDs one term into the query - Shown(), set by SetView. It is how a
        // second graph instance (a preview) drops what its view does not draw, and how
        // the main view drops the environment in wireframe and the debug views:
        // SkyboxPass and the preview's passes have no IsActive() of their own, and
        // giving them one would change every graph (same classes).

        // The board's reader is waiting for a publish, so EVERY exit taken
        // after ConsumeRequest calls this - the nothing-active early-out too
        // (@p recorded false: no CL, record time or wait exists this frame).
        auto publishSnapshot = [&](bool recorded)
        {
            std::shared_ptr<GraphSnapshot> snap =
                BuildSnapshot(gfx, recorded, snapBarriers, snapUavBarriers, snapInitial, snapInitialStencil);
            // The DAG of this frame: the numbers the rg_dag pack reads.
            LOG_INFO("[RG_DAG] frame=%llu active=%u edges=%zu raw=%u war=%u waw=%u staleReads=%u "
                     "inactiveProducers=%u conflicts=%u clearAfterLive=%u stencilOverlaps=%u opaquePasses=%u "
                     "uavPairs=%u uavBarriers=%u findings=%zu deriveUs=%.1f",
                     static_cast<unsigned long long>(m_snapshotFrame), snap->RanCount(),
                     snap->edges.size(), m_dag.raw, m_dag.war, m_dag.waw,
                     snap->FindingCount(GraphSnapshot::Check::StaleRead),
                     snap->FindingCount(GraphSnapshot::Check::InactiveProducer),
                     snap->FindingCount(GraphSnapshot::Check::ConflictInPass),
                     snap->FindingCount(GraphSnapshot::Check::ClearAfterLive),
                     snap->FindingCount(GraphSnapshot::Check::StencilOverlap),
                     snap->FindingCount(GraphSnapshot::Check::OpaquePass),
                     snap->FindingCount(GraphSnapshot::Check::UavNeedsBarrier),
                     snap->UavBarrierCount(), snap->findings.size(), snap->deriveUs);
            const GraphSnapshot& s = *snap;
            const size_t passes = s.passes.size(), textures = s.textures.size(), edges = s.edges.size();
            const uint32_t active = s.RanCount(), backed = s.BackedCount(), barriers = s.BarrierCount();
            const uint32_t waits = s.WaitCount(), auxTails = s.AuxTailCount(), undeclared = s.UndeclaredCount();
            const long long gpuFrame = static_cast<long long>(s.gpuFrame);
            std::string unread;   // the cull candidates' names
            for (const GraphSnapshot::Pass& p : s.passes)
                if (p.unread) unread += (unread.empty() ? "" : ",") + p.name;
            const uint32_t candidates = s.UnreadCount();
            if (journaled) m_frameCaptureSnapshot = snap;
            m_snapshotBoard->Publish(std::move(snap));
            // Proof of life, and what the rg_snapshot pack reads.
            LOG_INFO("[RG_SNAPSHOT] frame=%llu seq=%llu passes=%zu active=%u textures=%zu backed=%u "
                     "edges=%zu barriers=%u waits=%u auxTails=%u undeclared=%u gpuFrame=%lld",
                     static_cast<unsigned long long>(m_snapshotFrame),
                     static_cast<unsigned long long>(m_snapshotBoard->PublishedSequence()),
                     passes, active, textures, backed, edges, barriers, waits, auxTails, undeclared,
                     gpuFrame);
            // The cull report: what culling would remove - nothing is.
            LOG_INFO("[RG_CULL] frame=%llu active=%u candidates=%u passes=%s",
                     static_cast<unsigned long long>(m_snapshotFrame), active, candidates,
                     unread.empty() ? "-" : unread.c_str());
        };

        // ---- Per-frame tick, BEFORE the gate ----
        // RenderPass::PrepareFrame runs for every registered pass, hidden or inactive,
        // so per-frame bookkeeping still happens on the frames the pass does not
        // record. The base is empty: one virtual call per pass per frame, a no-op for
        // everything that does not opt in. In a secondary view only for the passes that
        // record there (kViewSafe): another pass's per-frame state is the main view's, and
        // a tick here would reset it after the main view set it.
        for (size_t i = 0; i < passCount; ++i)
            if (m_boundSet == 0 || (m_passes[i].pass->HandledEvents() & PassLifecycle::kViewSafe))
                m_passes[i].pass->PrepareFrame();

        auto& active = m_activePasses;
        active.resize(passCount);
        size_t activeCount = 0;
        size_t lastActive  = 0;
        size_t hiddenCount = 0;
        std::string viewSkipped;
        for (size_t i = 0; i < passCount; ++i)
        {
            // A pass the view does not show is skipped structurally, whatever IsActive
            // says, -passactiveoff or not (SetView set Shown()); so, in a secondary
            // view, is one that keeps per-view state outside the view (kViewSafe).
            const bool shown = m_passes[i].pass->Shown();
            hiddenCount += shown ? 0u : 1u;
            const bool viewSafe = m_boundSet == 0 ||
                                  (m_passes[i].pass->HandledEvents() & PassLifecycle::kViewSafe);
            if (shown && !viewSafe)
            {
                if (!viewSkipped.empty()) viewSkipped += ' ';
                viewSkipped += m_passes[i].pass->InstanceName();
            }
            active[i] = (shown && viewSafe && m_passes[i].pass->IsActive()) ? uint8_t{1} : uint8_t{0};
            if (active[i]) { ++activeCount; lastActive = i; }
        }
        if (m_boundSet != 0 && viewSkipped != m_viewSkipLogged)
        {
            m_viewSkipLogged = viewSkipped;
            LOG_INFO("[VIEW] slot=%u skipped (not view-safe): %s", m_boundSet,
                     viewSkipped.empty() ? "-" : viewSkipped.c_str());
        }

        // [RG_ACTIVE] - how many of the registered passes ran. Rate-limited like
        // [RENDERZONE]: -phaselog_every, default 60 when the flag is present without a
        // value, silent when it is absent. (m_commandListPeak cannot answer this: a
        // monotonic high-water mark that logs only while rising cannot see a drop.)
        // legacy= is the same sweep with the activity gates switched off, so one run
        // gives both counts with no paired capture and no drift between binaries.
        // IsActive() is side-effect free, so re-querying it is harmless; it is done
        // only on log frames. hidden= is how many passes the view does not show (they
        // count in neither number).
        {
            static const int s_activeLogEvery = []{
                const auto& cl = Sys::CommandLine::Get();
                return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
            }();
            // passes= tells the instances apart (the main graph registers ~48 passes, a
            // preview GraphSet 5); RenderGraph has no graph name to print.
            if (s_activeLogEvery > 0 && ++m_activeLogFrames % s_activeLogEvery == 0)
            {
                const bool gatesWere = PassActivityGatesEnabled();
                SetPassActivityGatesEnabled(false);
                size_t legacyCount = 0;
                for (size_t i = 0; i < passCount; ++i)
                    if (m_passes[i].pass->Shown() && m_passes[i].pass->IsActive())
                        ++legacyCount;
                SetPassActivityGatesEnabled(gatesWere);

                LOG_INFO("[RG_ACTIVE] active=%zu legacy=%zu passes=%zu hidden=%zu",
                         activeCount, legacyCount, passCount, hiddenCount);
            }
        }

#if WITH_HARNESS
        // ---- -passactiveverify[=N]: the activity-gate oracle ----
        // Two invariants a bad override breaks and a golden image cannot see on a
        // frame where the pass happened to draw nothing anyway:
        //
        //   subset=1     the gate only ever REMOVES passes: every pass active with
        //                the gates on is also active with them off. A predicate
        //                NARROWER than the pass's own early-out costs work; a WIDER
        //                one (an inverted test, a stale cached flag) makes a pass
        //                record that the ungated picture skipped - a rendering
        //                change, not a performance one.
        //   stable=1     IsActive() is idempotent within a frame. A predicate that
        //                ticks a counter or lazily creates something breaks Phase
        //                A/B/C agreement, and this is the only place that can catch
        //                it cheaply.
        //
        // Both are ENGINE_CHECKs as well as counters, so -maxerrors=0 fails the run
        // rather than leaving the numbers for someone to read.
        {
            static const int s_verifyEvery = []{
                const auto& cl = Sys::CommandLine::Get();
                return cl.Has("-passactiveverify") ? cl.GetInt("-passactiveverify", 60) : 0;
            }();
            if (s_verifyEvery > 0 && ++m_activeVerifyFrames % uint64_t(s_verifyEvery) == 0)
            {
                uint32_t widened = 0, unstable = 0, gated = 0;
                // A pass the view does not show is the structural gate's - not the activity
                // gate's to answer for.
                auto structural = [&](size_t i) { return !m_passes[i].pass->Shown(); };
                for (size_t i = 0; i < passCount; ++i)
                {
                    if (structural(i)) continue;
                    const bool second = m_passes[i].pass->IsActive();
                    if (second != (active[i] != 0)) ++unstable;
                }
                const bool gatesWere = PassActivityGatesEnabled();
                SetPassActivityGatesEnabled(false);
                for (size_t i = 0; i < passCount; ++i)
                {
                    if (structural(i)) continue;
                    const bool legacy = m_passes[i].pass->IsActive();
                    if (active[i] && !legacy) ++widened;
                    if (!active[i] && legacy) ++gated;
                }
                SetPassActivityGatesEnabled(gatesWere);

                ENGINE_CHECK_ONCE(widened == 0,
                    "RenderGraph pass-activity oracle: %u pass(es) are active WITH the "
                    "honest gates and inactive without them - an IsActive() override is "
                    "wider than the early-out it mirrors", widened);
                ENGINE_CHECK_ONCE(unstable == 0,
                    "RenderGraph pass-activity oracle: %u pass(es) answered IsActive() "
                    "differently twice in one frame - the query is not side-effect free",
                    unstable);

                LOG_INFO("[PASSACTIVE] active=%zu gated=%u passes=%zu subset=%u stable=%u",
                         activeCount, gated, passCount,
                         widened == 0 ? 1u : 0u, unstable == 0 ? 1u : 0u);
            }
        }
#endif

        // ---- This frame's DAG of the active passes ----
        // Main graph only (the one that publishes snapshots); serial, before anything
        // records. Microseconds: a few dozen passes x a handful of declarations,
        // scratch reused (RenderGraph/RenderGraphDag.h).
        if (m_snapshotBoard) DeriveFrameDag();

        if (activeCount == 0)
        {
            // Reached through IsActive(), not through the view: GBufferPass and
            // LightingPass declare no ShowFlag, so a view cannot hide every pass -
            // `active` is all 0 only when every shown pass reported itself inactive, which
            // a main graph of self-gating passes can do and a preview's (passes that never
            // override IsActive()) cannot.
            // Do not reset the view here: it is sticky (see SetView), and a reset would
            // show every pass on the next frame. Only the per-frame external-wait map is
            // dropped.
            m_externalWaits.clear();
            if (snapshot) publishSnapshot(false);
            co_return;
        }

        // ---- Re-home external cross-queue waits off inactive passes ----
        // SetExternalWait targets a pass BY NAME. If that pass is skipped this frame
        // the wait would silently disappear - and it is the fence that makes a
        // compute-queue producer (DDGI) finish before its graphics consumer reads the
        // result. Attach it to the next ACTIVE pass instead; the wait lands slightly
        // earlier than intended, which is conservative and correct.
        auto& extWaits = m_executeWaits;
        extWaits.resize(passCount);
        for (auto& waits : extWaits) waits.clear();
        if (!m_externalWaits.empty())
        {
            for (size_t i = 0; i < passCount; ++i)
            {
                auto it = m_externalWaits.find(m_passes[i].pass->InstanceName());
                if (it == m_externalWaits.end() || !it->second.IsValid()) continue;

                size_t target = i;
                while (target < passCount && !active[target]) ++target;
                if (target >= passCount)
                {
                    if (!m_extWaitDroppedLogged)
                    {
                        m_extWaitDroppedLogged = true;
                        LOG_WARNING("RenderGraph: external wait registered for pass "
                                    "'%s', but neither it nor any later pass is "
                                    "active this frame — the cross-queue dependency "
                                    "is DROPPED. (logged once per session)",
                                    m_passes[i].pass->InstanceName());
                    }
                    continue;
                }
                if (target != i && !m_extWaitRehomeLogged)
                {
                    m_extWaitRehomeLogged = true;
                    LOG_WARNING("RenderGraph: pass '%s' is inactive; its external "
                                "cross-queue wait was re-homed onto the next active "
                                "pass '%s'. (logged once per session)",
                                m_passes[i].pass->InstanceName(),
                                m_passes[target].pass->InstanceName());
                }
                extWaits[target].push_back(it->second);
            }
        }

        // No texture enters this Execute owed a UAV barrier: the previous frame's
        // lists went out in other ExecuteCommandLists calls, and those boundaries
        // flush.
        m_uavLastPass.assign(m_textures.size(), -1);

        // ---- The texture copy armed for this Execute ----
        // Taken here, once; copyAfter = the pass the copy follows (SIZE_MAX: none this
        // frame). Phase A records it before the next active pass, Phase C after the
        // tail when no active pass follows (ArmTextureCopy).
        size_t           copyAfter = SIZE_MAX;
        RHI::CommandList copyCL{};
        TextureCopyArm   copyArm;
        if (m_copyArm.armed)
        {
            copyArm = std::exchange(m_copyArm, TextureCopyArm{});
            TextureCopyResult& res = m_copyResult;
            res          = TextureCopyResult{};
            res.armed    = true;
            res.frame    = m_snapshotFrame;
            copyArm.handle = FindTexture(copyArm.texture);
            const size_t after = copyArm.afterPass.empty() ? lastActive : FindPass(copyArm.afterPass);
            res.afterPass = after < passCount ? std::string(m_passes[after].pass->InstanceName()) : copyArm.afterPass;
            const VirtualTexture* vt = copyArm.handle.IsValid() ? &m_textures[copyArm.handle.id] : nullptr;
            const RHI::Texture* dst = copyArm.dst;
            if (snapshot)
            {
                res.deferred = true;
                res.why      = "a snapshot frame (its journal would carry the copy)";
            }
            else if (!vt)
                res.why = "no graph texture '" + copyArm.texture + "'";
            else if (!vt->texture.IsValid())
                res.why = "the texture is not backed in this run";
            else if (vt->OwnerTracked())
                res.why = "an owner-tracked import: its owner moves its state, the graph does not know it";
            else if (after >= passCount)
                res.why = "no pass '" + copyArm.afterPass + "'";
            else if (!active[after])
                res.why = std::string("pass '") + m_passes[after].pass->InstanceName() + "' did not run this frame";
            else if (!dst || !dst->IsValid() || !copyArm.dstState)
                res.why = "no copy texture";
            else if (dst->desc.width != vt->texture.desc.width || dst->desc.height != vt->texture.desc.height ||
                     dst->desc.format != vt->texture.desc.format ||
                     dst->desc.arraySize != vt->texture.desc.arraySize ||
                     dst->desc.mipLevels != vt->texture.desc.mipLevels)
                res.why = "the copy texture does not match the texture (it was rebuilt) - the next frame";
            else
                copyAfter = after;
        }

        // While one lives, the command lists this thread begins belong to the pass:
        // Phase C sums their draw / dispatch / PSO / barrier counts onto its profiler
        // region.
        struct RecordingOwnerScope
        {
            IGraphicsDevice& gfx;
            RecordingOwnerScope(IGraphicsDevice& g, const RenderPass* pass) : gfx(g)
            { gfx.SetRecordingOwner(reinterpret_cast<uintptr_t>(pass)); }
            ~RecordingOwnerScope() { gfx.SetRecordingOwner(0); }
        };

        // ---- Phase A: allocate CLs + barriers (serial) ----
        bool viewSetActive = false;   // ActivateBoundSet ran (a secondary view)
        for (size_t i = 0; i < passCount; ++i)
        {
            ZoneScopedN("RenderGraph.PassSetup");
            ZoneValue(i);
            PROF_SCOPE("RenderGraph.PassSetup");
            auto& node = m_passes[i];
            PassRecord& r = recs[i];
            r.profilerRegion = ~0u;

            // Inactive: no CL, no target bind, no barriers, no timestamp, no marker - and
            // prevCL is NOT advanced, so the dependency chain links the surrounding ACTIVE
            // passes directly.
            if (!active[i]) continue;

            // The armed copy, on a list of its own between the pass it follows and this
            // one - recorded before this pass's barriers, in the order the GPU runs them
            // (Phase C orders the pass's aux tail before it).
            if (copyAfter < i && !copyCL.IsValid())
            {
                copyCL = RecordTextureCopy(gfx, prevCL, copyArm);
                prevCL = copyCL;
            }

            // The pass's list and the chunk lists PrepareRecording opens below.
            RecordingOwnerScope owner{ gfx, m_passes[i].pass.get() };
            r.cl = gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
            // A secondary view's placed textures, active from here (before any barrier of its passes).
            if (m_boundSet != 0 && !viewSetActive)
            {
                ActivateBoundSet(gfx, r.cl);
                viewSetActive = true;
            }

            // DX12_DEPTHTRACE roster: pass name -> pool slot, emitted from the
            // SERIAL phase so the log's order is the recording order. GetName()
            // outlives the frame (it returns the pass's own storage), which is
            // what the trace table requires.
            GfxTrace::Label(r.cl, m_passes[i].pass->InstanceName());
            // "cmd<id>:<pass>" for DRED reports and captures (no-op unless markers or
            // DRED are on).
            gfx.SetCommandListName(r.cl, m_passes[i].pass->InstanceName());

            if (prevCL.IsValid())
                gfx.AddCommandListDependency(r.cl, prevCL);

            // External cross-queue wait (e.g. LightingPass <- DDGI compute CL),
            // registered by the caller via SetExternalWait before Execute. Landing it
            // here keeps GBuffer / Terrain / SkyIBL running in parallel with the async
            // producer while only the dependent pass stalls.
            // Do not also add the reverse edge (producer <- prevCL, earliest consumer
            // only) to pin the producer CL next to its consumer instead of letting
            // EndFrame's Kahn BFS hoist it to the front of the graphics submit order: it
            // should widen the async-compute overlap but measured worse. Re-apply only
            // with a profile that shows a win.
            if (!extWaits.empty())
            {
                for (const RHI::CommandList& dep : extWaits[i])
                    gfx.AddCommandListDependency(r.cl, dep);
            }

            if (node.hdrShim.IsValid())
            {
                // The shim declared the HDR import, so its entry transition is one of this
                // pass's graph barriers - emitted FIRST, in the same batch - and the builtin
                // bind that follows finds it in RENDER_TARGET. HdrSceneColor clears to the
                // import's value (BindImport).
                EmitBarriersBeforePass(gfx, r.cl, node, static_cast<uint32_t>(i),
                                       snapshot ? &snapBarriers[i] : nullptr,
                                       snapshot ? &snapUavBarriers[i] : nullptr);
                if (node.colorTarget == BuiltinTexture::HdrSceneColor)
                    gfx.SetRenderTargetToHdr(ImportClearValue(node.hdrShim), r.cl);
                else
                    gfx.SetRenderTargetToHdrWithDepth(nullptr, r.cl);   // bind HDR, no clear
            }
            else
            {
                // No import (-rgimportoff): the builtin bind transitions HDR itself, then the
                // graph's barriers. A null clear colour = the slot's stored one
                // (SetHdrClearColor).
                if (node.colorTarget == BuiltinTexture::HdrSceneColor)
                    gfx.SetRenderTargetToHdr(nullptr, r.cl);
                else if (node.colorTarget == BuiltinTexture::HdrSceneColorPreserve)
                    gfx.SetRenderTargetToHdrWithDepth(nullptr, r.cl);   // bind HDR, no clear
                else if (node.colorTarget == BuiltinTexture::SwapChainColor)
                    gfx.MainSurface().SetRenderTargetToSwapChain(clearColor, r.cl);

                EmitBarriersBeforePass(gfx, r.cl, node, static_cast<uint32_t>(i),
                                       snapshot ? &snapBarriers[i] : nullptr,
                                       snapshot ? &snapUavBarriers[i] : nullptr);
            }

            r.cl.gfx = &gfx;
            r.cl.ctx = &ctx;

            // Profiler region only. Its index comes from an allocation counter, so it
            // must stay serial here or the editor HUD's row order would shuffle every
            // frame. The matching TRACY zone is opened and closed inside Phase B, around
            // the pass's own recording - see IGraphicsDevice::BeginGPUTimestampNoTracy for
            // why it cannot be opened here.
            r.profilerRegion = gfx.BeginGPUTimestampNoTracy(
                r.cl, m_passes[i].pass->InstanceName());

            // Name the pass for GPU-capture tools (RenderDoc/PIX). Called for
            // every active pass, independent of the profiler; the device only
            // records it while markers are on (-gpumarkers, DX12_MARKERS=1, or a
            // capture tool loaded at startup). The timestamp above is the
            // NoTracy kind, which emits no marker, so the pass gets one region.
            gfx.BeginEventMarker(r.cl, m_passes[i].pass->InstanceName());

            const uint32_t extras = node.pass->PrepareRecording(r.cl);
            for (uint32_t task = 0; task < extras; ++task)
                extraTasks.emplace_back(static_cast<uint32_t>(i), task);

            prevCL = r.cl;
        }

#if RG_VALIDATE_ACCESS
        // `ctx` is a LOCAL, rebuilt every Execute, so this must be installed every
        // frame, not once (an install-once guard would arm only the first frame); it
        // is a pointer store. The undeclared-access store is indexed BY PASS;
        // OnTextureAccess locks only on an undeclared access, which split recording
        // can report concurrently.
        ctx.SetAccessValidator(&RenderGraph::AccessValidatorThunk, this);
        ctx.SetDepthViewLookup(&RenderGraph::DepthViewThunk, this);   // the DepthStencil views (OnDepthView)
        if (m_undeclaredByPass.size() != m_passes.size())
        {
            m_undeclaredByPass.assign(m_passes.size(), {});
            m_reportedByPass.assign(m_passes.size(), 0);
        }
#endif

        // ---- Phase B: record each pass in parallel ----
        // Bracket the whole window (both branches - the serial fallback keeps the flag
        // balanced) so the backend can flag any pass that mutates a shared device-side
        // resource-state tracker from inside Execute(). Every pass recording at the
        // same instant reads those trackers, so a mid-flight change makes other passes
        // emit barriers whose before-state does not match the GPU's. See
        // GraphicsDX12::WarnOnRacyHdrStateChange.
        // The clear is RAII-backed: a throwing Execute() is rethrown on this thread,
        // which would skip a bare SetParallelPassRecording(false) and leave every
        // later frame diagnosed as racy. End() drops the flag the moment Phase B is
        // done; the destructor fires only on the throwing path.
        struct ParallelRecordingScope
        {
            IGraphicsDevice* g = nullptr;
            ~ParallelRecordingScope() { if (g) g->SetParallelPassRecording(false); }
            void End() { if (g) { g->SetParallelPassRecording(false); g = nullptr; } }
        } parallelRecordingScope{ &gfx };
        gfx.SetParallelPassRecording(true);
        const uint32_t workCount = static_cast<uint32_t>(passCount + extraTasks.size());
        m_recordCostMs.resize(workCount, 0.0);
        m_recordSamplesMs.assign(workCount, 0.0);
        auto recordWork = [&](uint32_t work) {
            const bool additional = work >= passCount;
            const uint32_t i = additional ? extraTasks[work - passCount].first : work;
            if (!active[i]) return;
            RecordingOwnerScope owner{ gfx, m_passes[i].pass.get() };
            // One writer per work item, sampled on both record paths. The previous
            // frames' costs only order the CPU work, never GPU dependencies.
            struct RecordSample {
                double& sample;
                std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
                ~RecordSample() { sample = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count(); }
            } sample{m_recordSamplesMs[work]};
#if RG_VALIDATE_ACCESS
            struct AccessScope {
                int& slot; int old;
                ~AccessScope() { slot = old; }
            } accessScope{ s_tlsCurrentPass, s_tlsCurrentPass };
            struct GraphScope {
                const RenderGraph*& slot; const RenderGraph* old;
                ~GraphScope() { slot = old; }
            } graphScope{ s_tlsCurrentGraph, s_tlsCurrentGraph };
            s_tlsCurrentPass  = static_cast<int>(i);
            s_tlsCurrentGraph = this;
#endif
            if (additional)
            {
                ZoneScopedN("RenderGraph.ExtraRecordTask");
                ZoneValue(extraTasks[work - passCount].second);
                PROF_SCOPE("RenderGraph.ExtraRecordTask");
                m_passes[i].pass->ExecuteAdditionalRecording(extraTasks[work - passCount].second);
                return;
            }
            const char* pn = m_passes[i].pass->InstanceName();
#ifdef TRACY_ENABLE
            ZoneTransientN(passZone, pn, true);
#endif
            // Per-pass CPU RECORD time, on the worker that recorded it.
            PROF_SCOPE_DYN(pn);
            gfx.BeginTracyGPUZone(recs[i].cl, pn, recs[i].profilerRegion);
            const auto cpuBegin = std::chrono::steady_clock::now();
            try {
                recs[i].returnedCL = m_passes[i].pass->ExecutePrepared(recs[i].cl);
            } catch (...) {
                gfx.EndTracyGPUZone(recs[i].profilerRegion);
                throw;
            }
            // Split passes report the primary chunk here; additional chunk
            // work is visible in its own CPU/GPU zones, not added as latency.
            if (profileEvery > 0) passMs[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuBegin).count();
            gfx.EndTracyGPUZone(recs[i].profilerRegion);
        };
        if (workCount >= 4)
        {
            auto& order = m_recordOrder;
            order.clear();
            for (uint32_t work = 0; work < workCount; ++work) {
                const uint32_t pass = work < passCount ? work : extraTasks[work - passCount].first;
                if (active[pass]) order.push_back(work);
            }
            if (graphRecording)
                std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                    return m_recordCostMs[a] != m_recordCostMs[b]
                        ? m_recordCostMs[a] > m_recordCostMs[b] : a < b;
                });
            std::atomic<uint32_t> next{0};
            auto drain = [&] {
                ZoneScopedN("RenderGraph.RecordLane");
                PROF_SCOPE("RenderGraph.RecordLane");
                for (uint32_t index = next.fetch_add(1, std::memory_order_relaxed);
                     index < order.size(); index = next.fetch_add(1, std::memory_order_relaxed))
                    recordWork(order[index]);
            };
            // GBuffer's primary and additional chunks share one flat work list: any lane
            // may run on any worker, a one-worker pool included; there is no nested
            // recording latch.
            const uint32_t lanes = (std::max)(1u, (std::min)(TaskSystem::Get().GetWorkerCount(),
                static_cast<uint32_t>((order.size() + 1) / 2)));
            std::vector<Sys::TaskHandle> handles(lanes);
            Sys::CpuTaskScope recording(lanes); // exception-only drain of borrowed frame data
            for (uint32_t lane = 0; lane < lanes; ++lane) handles[lane] = recording.Submit(drain);
            if (m_recordingReady) m_recordingReady();
            std::exception_ptr error;
            for (const auto& handle : handles)
                try { co_await handle; } catch (...) { if (!error) error = std::current_exception(); }
            recording.Join(); // all handles ready; release captures before Phase C
            if (error) std::rethrow_exception(error);
        }
        else
            for (uint32_t work = 0; work < workCount; ++work) recordWork(work);
        parallelRecordingScope.End();   // == gfx.SetParallelPassRecording(false)
        for (uint32_t work = 0; work < workCount; ++work)
            m_recordCostMs[work] = m_recordCostMs[work] * 0.75 + m_recordSamplesMs[work] * 0.25;
        for (size_t i = 0; i < passCount; ++i)
            if (active[i]) m_passes[i].pass->FinishRecording();
        if (profileEvery > 0)
        {
            if (m_cpuPassMs.size() != passMs.size()) m_cpuPassMs.assign(passMs.size(), 0.0);
            for (size_t i = 0; i < passMs.size(); ++i) m_cpuPassMs[i] += passMs[i];
            if (++m_cpuProfileFrames % profileEvery == 0)
                for (size_t i = 0; i < passMs.size(); ++i)
                {
                    if (m_cpuPassMs[i] / profileEvery >= 0.1)
                        LOG_INFO("[PASSCPU] %s %.3f ms", m_passes[i].pass->InstanceName(), m_cpuPassMs[i] / profileEvery);
                    m_cpuPassMs[i] = 0;
                }
        }

#if RG_VALIDATE_ACCESS
        ReportUndeclaredAccess();
#endif

        // ---- Phase C: close profiler regions (serial, CL writes) ----
        // NOT wrapped in IsGPUProfilerEnabled(): Phase A allocated these regions
        // against the profiler's state at THAT moment. Toggled off mid-frame, the
        // wrapper would skip the closes and leave half-open regions whose
        // EndQuery/ResolveQueryData would fire into an already-executed command list
        // next frame - device removal. EndGPUTimestampNoTracy no-ops on the ~0u regions
        // a disabled profiler hands back, so calling it unconditionally is correct and
        // cheap. The Tracy half was closed in Phase B.
        for (size_t i = 0; i < passCount; ++i)
            if (active[i]) gfx.EndGPUTimestampNoTracy(recs[i].cl, recs[i].profilerRegion);
        // Every list the pass recorded - its own and the chunk lists it opened - onto
        // its region (the end above counted its own list).
        for (size_t i = 0; i < passCount; ++i)
            if (active[i] && recs[i].profilerRegion != ~0u)
                gfx.AnnotateGPUTimestamp(recs[i].profilerRegion,
                                         gfx.GetOwnerCounters(reinterpret_cast<uintptr_t>(m_passes[i].pass.get())));

        // Close the per-pass debug-marker regions (profiler-independent; balances
        // the BeginEventMarker issued for every ACTIVE pass in Phase A, and is a
        // no-op exactly when that Begin was).
        for (size_t i = 0; i < passCount; ++i)
            if (active[i]) gfx.EndEventMarker(recs[i].cl);

        // ---- Phase C: order pass-owned auxiliary command lists (serial) ----
        // RenderPass::Execute returns the CL it FINISHED recording on, which may not
        // be the one the graph allocated: GBufferPass opens worker CLs for its draw
        // chunks and returns the last of them. The next ACTIVE pass waits on that aux
        // tail; without the edge those lists land in order only by an accident of
        // pool-index ordering in EndFrame's Kahn sort. Chaining WITHIN a pass's own
        // aux CLs is that pass's job (see GBufferPass). Serial on purpose: never add
        // graph-side dependency edges from inside the parallel recording.
        for (size_t i = 0; i < passCount; ++i)
        {
            if (!active[i]) continue;
            const RHI::CommandList ret = recs[i].returnedCL;
            // Trips when a pass forgets to return a CL from Execute: everything it
            // recorded past that point would be unordered against the graph.
            ENGINE_CHECK_ONCE(ret.IsValid(),
                "RenderPass '%s': Execute must return the command list it finished on",
                m_passes[i].pass->InstanceName());
            if (!ret.IsValid() || ret == recs[i].cl) continue;

            // The copy that follows this pass waits on its tail too.
            if (i == copyAfter && copyCL.IsValid())
                gfx.AddCommandListDependency(copyCL, ret);

            size_t next = i + 1;
            while (next < passCount && !active[next]) ++next;
            if (next < passCount)
                gfx.AddCommandListDependency(recs[next].cl, ret);
        }

        // External waits are per-frame; clear so a stale dep from a previous
        // frame doesn't leak into the next graph run.
        m_externalWaits.clear();

        // The tail is the last active pass's RETURNED CL - that is what callers must
        // chain onto, not the CL the graph handed it.
        const RHI::CommandList tail = recs[lastActive].returnedCL;
        result = tail.IsValid() ? tail : recs[lastActive].cl;
        // A copy after the last pass that ran goes after the graph's tail - and is
        // the tail the caller chains onto.
        if (copyAfter != SIZE_MAX && !copyCL.IsValid())
        {
            copyCL = RecordTextureCopy(gfx, result, copyArm);
            result = copyCL;
        }
        if (snapshot) publishSnapshot(true);
        co_return;
    }

    // ---- The executed frame as data ----
    // Serial, at the end of Execute (or at its nothing-active early-out). Reads
    // the scratch that Execute leaves in place until the next one: the activity
    // bytes, the pass records, the landed waits, the record samples.
    std::shared_ptr<GraphSnapshot> RenderGraph::BuildSnapshot(
        const IGraphicsDevice& gfx, bool recorded,
        const std::vector<std::vector<GraphSnapshot::Barrier>>& barriers,
        const std::vector<std::vector<uint32_t>>& uavBarriers,
        const std::vector<uint32_t>& initialStates,
        const std::vector<uint32_t>& initialStencilStates) const
    {
        using Snap = GraphSnapshot;
        auto snap = std::make_shared<GraphSnapshot>();
        Snap& s = *snap;
        s.frame  = m_snapshotFrame;
        s.width  = m_physicalWidth;    // what the graph was compiled at
        s.height = m_physicalHeight;
        s.viewName = m_view.name ? m_view.name : "";
        s.viewShow = m_view.show.bits;
        s.viewCullKind = static_cast<uint8_t>(m_view.cull.volume.kind);
        for (int i = 0; i < 3; ++i) s.viewEye[i] = m_view.cull.eye[i];
        s.viewBit = m_view.cull.viewBit;

        // ---- passes, in declaration (= execution) order ----
        const size_t passCount = m_passes.size();
        s.passes.resize(passCount);
        for (size_t i = 0; i < passCount; ++i)
        {
            const PassNode& n = m_passes[i];
            Snap::Pass& p = s.passes[i];
            p.name  = n.pass ? n.pass->InstanceName() : "<null>";
            p.index = static_cast<uint32_t>(i);
            const bool ran     = i < m_activePasses.size() && m_activePasses[i] != 0;
            const bool shown   = !n.pass || n.pass->Shown();
            const bool viewed  = m_boundSet == 0 || !n.pass ||
                                 (n.pass->HandledEvents() & PassLifecycle::kViewSafe);
            p.skip        = ran ? Snap::Skip::None
                          : !shown ? Snap::Skip::Hidden : !viewed ? Snap::Skip::View : Snap::Skip::Inactive;
            p.showFlags   = n.pass ? n.pass->RequiredShowFlags() : 0u;
            p.colorTarget = ColorTargetName(n.colorTarget);
            p.stage       = StageName(n.stage);
            p.registered  = n.registered;
            p.order       = n.order;

            // The order EmitBarriersBeforePass transitions in: rt, ds, uav, srv, then
            // the manual handles (built once per Compile).
            if (i < m_passAccesses.size()) p.accesses = m_passAccesses[i];

            if (i < barriers.size())    p.barriers    = barriers[i];
            if (i < uavBarriers.size()) p.uavBarriers = uavBarriers[i];
            if (recorded && ran)
            {
                if (i < m_executeWaits.size())
                    p.externalWaits = static_cast<uint32_t>(m_executeWaits[i].size());
                if (i < m_passRecords.size())
                {
                    const PassRecord& r = m_passRecords[i];
                    p.cl = r.cl.internalId;
                    if (r.returnedCL.IsValid() && !(r.returnedCL == r.cl))
                        p.auxTailCl = r.returnedCL.internalId;
                }
                if (i < m_recordSamplesMs.size())
                    p.cpuMs = static_cast<float>(m_recordSamplesMs[i]);
            }
#if RG_VALIDATE_ACCESS
            if (i < m_undeclaredByPass.size()) p.undeclared = m_undeclaredByPass[i];
#endif
        }
        // A split pass's extra recording tasks (GBuffer / Transparent chunks)
        // are work items past the pass count; they count toward their pass.
        if (recorded)
        {
            for (size_t w = 0; w < m_extraRecordTasks.size(); ++w)
            {
                const size_t   work  = passCount + w;
                const uint32_t owner = m_extraRecordTasks[w].first;
                if (work < m_recordSamplesMs.size() && owner < passCount)
                    s.passes[owner].cpuMs += static_cast<float>(m_recordSamplesMs[work]);
            }
        }

        // ---- textures ----
        const size_t texCount = m_textures.size();
        s.textures.resize(texCount);
        for (size_t t = 0; t < texCount; ++t)
        {
            const VirtualTexture& vt = m_textures[t];
            Snap::Texture& x = s.textures[t];
            x.name         = vt.name;
            x.width        = vt.desc.width  ? vt.desc.width  : m_physicalWidth;
            x.height       = vt.desc.height ? vt.desc.height : m_physicalHeight;
            x.format       = static_cast<uint32_t>(vt.desc.format);
            x.arraySize    = vt.desc.arraySize;
            x.isDepth      = vt.desc.isDepth;
            x.isUAV        = vt.hasUAV;
            x.graphScope   = t < m_graphTextures.size();
            x.onDemand     = x.graphScope && m_graphTextures[t].residency == TextureResidency::OnDemand;
            x.imported     = vt.imported;
            x.importEntry    = static_cast<uint8_t>(vt.entry);
            x.importTracking = static_cast<uint8_t>(vt.tracking);
            // An import's resource carries the device's name, not the graph's - the
            // frame capture matches the journal by it - so ask the device, where the
            // journal reads it; the texture's desc.debugName is borrowed for its
            // CreateTexture call only (RTReflectionPass builds it in a temporary).
            if (vt.imported && vt.texture.IsValid())
                x.deviceName = gfx.QueryTextureName(vt.texture);
            x.backed       = vt.texture.IsValid();
            x.initialState = t < initialStates.size() ? initialStates[t]
                                                      : static_cast<uint32_t>(vt.currentState);
            x.finalState   = static_cast<uint32_t>(vt.currentState);
            // A depth-stencil texture's stencil plane.
            x.planar = vt.planar && PlanesEnabled();
            if (x.planar)
            {
                x.stencilInitialState = t < initialStencilStates.size() ? initialStencilStates[t]
                                                                        : static_cast<uint32_t>(vt.stencilState);
                x.stencilFinalState   = static_cast<uint32_t>(vt.stencilState);
            }
        }

        // ---- first / last use of each texture by a pass that RAN ----
        for (uint32_t i = 0; i < static_cast<uint32_t>(passCount); ++i)
        {
            const Snap::Pass& p = s.passes[i];
            if (!p.Ran()) continue;
            for (const Snap::PassAccess& a : p.accesses)
            {
                if (a.texture >= texCount) continue;
                Snap::Texture& x = s.textures[a.texture];
                if (x.firstUse < 0) x.firstUse = static_cast<int32_t>(i);
                x.lastUse = static_cast<int32_t>(i);
            }
        }

        // ---- dependency edges + findings: this frame's DAG ----
        // Derived before Phase A (DeriveFrameDag) from the same declarations and
        // activity bytes (RenderGraph/RenderGraphDag.h).
        s.edges    = m_dag.edges;
        s.findings = m_dag.findings;
        s.deriveUs = m_dagUs;

        // ---- the cull report: candidates only ----
        // A pass that ran and wrote, where every texture it wrote is the graph's own
        // (not an import - another frame or the world reads those - and not created
        // isSRV, which code outside the graph reads; isUAV only asks for the bind
        // flag) and no later pass read it this frame (no RAW edge out). [RG_CULL]
        // prints the list; the graph culls nothing (NeverCull).
        // TODO(RenderGraph_AutoTopology_NodeView_Plan M7): cull them once passes
        // declare their exports.
        for (uint32_t i = 0; i < static_cast<uint32_t>(passCount); ++i)
        {
            Snap::Pass& p = s.passes[i];
            if (!p.Ran()) continue;
            bool wrote = false, read = false;
            for (const Snap::PassAccess& a : p.accesses)
            {
                const bool writes = a.kind == Snap::Access::RenderTarget || a.kind == Snap::Access::Depth ||
                                    a.kind == Snap::Access::UAV || a.kind == Snap::Access::Manual ||
                                    a.kind == Snap::Access::CopyDst;
                if (!writes || a.texture >= texCount) continue;
                wrote = true;
                const VirtualTexture& vt = m_textures[a.texture];
                if (vt.imported || vt.desc.isSRV)
                    read = true;
                for (size_t e = 0; e < s.edges.size() && !read; ++e)
                    read = s.edges[e].from == i && s.edges[e].texture == a.texture &&
                           s.edges[e].kind == Snap::Edge::RAW;
                if (read) break;
            }
            p.unread = wrote && !read;
        }

        // ---- GPU time by pass name ----
        // From the newest GPU frame the engine profiler holds (GPU results come back
        // GPUProfiler::kFrameCount frames late; none at all unless GPU profiling is
        // on). Every span carrying the pass's name is summed; sub-regions have names
        // of their own.
        const uint64_t gpuFrame = Diag::Prof::LatestGpuFrame();
        if (gpuFrame != ~0ull)
        {
            auto data = std::make_unique<Diag::Prof::GpuFrameData>();   // ~24 KB: not on the stack
            if (Diag::Prof::GetGpuFrame(gpuFrame, *data) && data->valid)
            {
                s.gpuFrame = static_cast<int64_t>(gpuFrame);
                const uint32_t spans = (std::min)(data->spanCount, Diag::Prof::kMaxGpuSpans);
                for (uint32_t k = 0; k < spans; ++k)
                {
                    const Diag::Prof::GpuSpan& span = data->spans[k];
                    const char* name = Diag::Prof::NameOf(span.name);
                    if (!name || !*name) continue;
                    for (Snap::Pass& p : s.passes)
                    {
                        if (p.name != name) continue;
                        const float ms = static_cast<float>(static_cast<double>(span.endNs - span.beginNs) * 1e-6);
                        p.gpuMs = (p.gpuMs < 0.f ? 0.f : p.gpuMs) + ms;
                        break;
                    }
                }
            }
        }
        return snap;
    }

    // ==== RenderPass::ReloadShaders default: walks the accessor virtuals ====
    // Passes that need warm-up or extra state refresh override it.
    void RenderPass::ReloadShaders(IGraphicsDevice& /*gfx*/)
    {
        // A library that kept everything (ClearCaches drops only what the
        // edit reached) built PSOs that are still current: clearing them
        // would only retire PSO slots. A borrower names no library and always
        // clears its own caches.
        if (auto* lib = GetReloadableShaderLibrary())
            if (!lib->ClearCaches()) return;
        for (auto* cache : GetReloadablePSOCaches())
            if (cache) cache->Clear();
    }

    void RenderGraph::ReloadShaders(IGraphicsDevice& gfx)
    {
        // Each pass's override is the source of truth for what state needs
        // refreshing; the default RenderPass::ReloadShaders only clears the shader
        // library / PSO caches the pass's accessors report.
        for (auto& node : m_passes)
            node.pass->ReloadShaders(gfx);
        LOG_INFO("RenderGraph: shaders reloaded across %zu passes", m_passes.size());
    }

    bool RenderGraph::Contains(const RenderPass* pass) const
    {
        for (const auto& node : m_passes)
            if (node.pass.get() == pass) return true;
        return false;
    }

    // ==== The RenderPass census ====
    // Leaked on purpose: a pass can be destroyed after every static is (a member
    // of an object torn down after main).
    namespace
    {
        struct PassCensus
        {
            std::mutex mutex;
            uint64_t   next = 1;
            std::unordered_map<RenderPass*, uint64_t> live;   // pass -> construction number
        };
        PassCensus& Census()
        {
            static PassCensus* s_census = new PassCensus;
            return *s_census;
        }
    }

    RenderPass::RenderPass()
    {
        PassCensus& c = Census();
        std::lock_guard<std::mutex> lock(c.mutex);
        c.live[this] = c.next++;
    }

    RenderPass::RenderPass(const RenderPass&) : RenderPass() {}

    RenderPass::~RenderPass()
    {
        PassCensus& c = Census();
        std::lock_guard<std::mutex> lock(c.mutex);
        c.live.erase(this);
    }

    uint64_t PassCensusMark()
    {
        PassCensus& c = Census();
        std::lock_guard<std::mutex> lock(c.mutex);
        return c.next;
    }

    std::vector<RenderPass*> LivePassesSince(uint64_t mark)
    {
        std::vector<std::pair<uint64_t, RenderPass*>> found;
        {
            PassCensus& c = Census();
            std::lock_guard<std::mutex> lock(c.mutex);
            for (const auto& [pass, number] : c.live)
                if (number >= mark) found.emplace_back(number, pass);
        }
        std::sort(found.begin(), found.end());
        std::vector<RenderPass*> out;
        out.reserve(found.size());
        for (const auto& f : found) out.push_back(f.second);
        return out;
    }

#if RG_VALIDATE_ACCESS
    thread_local int                RenderGraph::s_tlsCurrentPass  = -1;
    thread_local const RenderGraph* RenderGraph::s_tlsCurrentGraph = nullptr;

    void RenderGraph::AccessValidatorThunk(void* user, RGTextureHandle h)
    {
        static_cast<RenderGraph*>(user)->OnTextureAccess(h);
    }

    // The recording pass's DSV view of @p h - what Phase A set down for its
    // DepthStencil declaration (read in Phase B, written only in the serial Phase
    // A before it).
    RHI::DepthView RenderGraph::DepthViewThunk(void* user, RGTextureHandle h)
    {
        return static_cast<const RenderGraph*>(user)->OnDepthView(h);
    }

    RHI::DepthView RenderGraph::OnDepthView(RGTextureHandle h) const
    {
        const int pi = s_tlsCurrentPass;
        if (pi < 0 || pi >= static_cast<int>(m_passes.size()) || !h.IsValid()) return {};
        for (const auto& v : m_passes[pi].depthViews)
            if (v.first == h.id) return v.second;
        return {};
    }

    // Called from RenderContext::GetTexture on the recording thread.
    void RenderGraph::OnTextureAccess(RGTextureHandle h)
    {
        const int pi = s_tlsCurrentPass;
        if (pi < 0 || pi >= static_cast<int>(m_passes.size())) return;   // outside Execute
        if (!h.IsValid()) return;

        const PassNode& node = m_passes[pi];
        auto declared = [&](const std::vector<RGTextureHandle>& v) {
            for (const RGTextureHandle& d : v) if (d.id == h.id) return true;
            return false;
        };
        if (declared(node.rtHandles)  || declared(node.dsHandles)  ||
            declared(node.srvHandles) || declared(node.uavHandles) ||
            declared(node.scopedHandles) ||
            declared(node.stencilSrvHandles) ||
            declared(node.manualHandles))   // explicit opt-out
            return;
        for (const PassNode::DsvDecl& d : node.dsvDecls) if (d.h.id == h.id) return;
        for (const auto& c : node.copySrc) if (c.first.id == h.id) return;
        for (const auto& c : node.copyDst) if (c.first.id == h.id) return;

        // Split recording can report the same undeclared access concurrently.
        // The normal declared-access path above remains lock-free.
        static std::mutex s_undeclaredMutex;
        std::lock_guard<std::mutex> lock(s_undeclaredMutex);
        if (pi >= static_cast<int>(m_undeclaredByPass.size())) return;
        auto& seen = m_undeclaredByPass[pi];
        for (uint32_t id : seen) if (id == h.id) return;   // already reported
        seen.push_back(h.id);
    }

    void RenderGraph::ReportUndeclaredAccess()
    {
        // One-shot proof of life: without it, "no violations logged" and "the
        // validator was compiled out / never armed" look identical in the log.
        if (!m_validationSummaryLogged)
        {
            m_validationSummaryLogged = true;
            size_t total = 0;
            for (const auto& v : m_undeclaredByPass) total += v.size();
            LOG_INFO("RenderGraph: undeclared-access validation ACTIVE over %zu "
                     "passes — %zu violation(s) on the first frame. 0 means every "
                     "pass declares every graph texture it touches.",
                     m_passes.size(), total);
        }

        for (size_t i = 0; i < m_undeclaredByPass.size(); ++i)
        {
            auto& seen = m_undeclaredByPass[i];
            if (seen.empty()) continue;
            // Log only entries not yet reported. `seen` doubles as the already-reported
            // set and must NOT be cleared: OnTextureAccess would re-detect the same pair
            // next frame and re-log it every frame. Runs serially after the parallel
            // recording, so the counter needs no lock.
            size_t& reported = m_reportedByPass[i];
            for (size_t k = reported; k < seen.size(); ++k)
            {
                const uint32_t id = seen[k];
                const char* tex = (id < m_textures.size())
                                ? m_textures[id].name.c_str() : "<unknown>";
                LOG_WARNING("RenderGraph: pass '%s' uses graph texture '%s' "
                            "WITHOUT declaring it in Setup() — the graph emits no "
                            "barrier for it, so this pass is order-dependent and "
                            "will break silently if passes are reordered. Add "
                            "ReadSRV/WriteRenderTarget/WriteUAV for it.",
                            m_passes[i].pass->InstanceName(), tex);
            }
            reported = seen.size();   // once per (pass, texture) per SESSION
        }
    }
#endif // RG_VALIDATE_ACCESS

    // ---- The stencil registry (RenderGraph/StencilBits.h) ----
    // Every Compile: each DepthStencil declaration's stencil bits - and a
    // stencil-plane SRV, which reads the whole byte - against the fields that
    // name its pass (StencilOwner: INFO, an ERROR under -rgdagverify), and the
    // census [RG_STENCIL] - per field, the passes of this graph that declare
    // its bits, active or not, the clear apart. Logged when it differs from
    // the last Compile's (a resize recompiles the same declarations); a graph
    // without a stencil declaration (-rgplaneoff, no depth-stencil texture)
    // prints nothing.
    void RenderGraph::CheckStencilOwners()
    {
        static const bool s_verify = Sys::CommandLine::Get().Has("-rgdagverify");
        constexpr size_t kFieldCount = std::size(StencilBits::kFields);
        std::string writers[kFieldCount], readers[kFieldCount], clearers;
        struct Outside { const char* pass; bool write; uint32_t bits, allowed; };
        std::vector<Outside> outside;
        uint32_t declarations = 0;
        auto add = [](std::string& list, const char* name)
        {
            if (("," + list + ",").find("," + std::string(name) + ",") != std::string::npos) return;
            if (!list.empty()) list += ',';
            list += name;
        };
        auto check = [&](const PassNode& node, bool write, uint8_t mask)
        {
            ++declarations;
            const char* name = node.pass->InstanceName();
            const std::string_view base = StencilBits::BaseName(node.pass->GetName());
            const bool clears = write && base == StencilBits::kClearedBy;
            for (size_t f = 0; f < kFieldCount; ++f)
            {
                const StencilBits::Field& field = StencilBits::kFields[f];
                if (!(mask & field.mask)) continue;
                if (clears && !StencilBits::Names(field.writers, base)) continue;   // the clear, listed apart
                add(write ? writers[f] : readers[f], name);
            }
            if (clears) add(clearers, name);
            const uint8_t allowed = write ? StencilBits::WriteMask(base) : StencilBits::ReadMask(base);
            if (mask & ~allowed)
                outside.push_back({ name, write, static_cast<uint32_t>(mask & ~allowed), allowed });
        };
        for (const PassNode& node : m_passes)
        {
            if (!node.pass) continue;
            for (const PassNode::DsvDecl& d : node.dsvDecls)
                if (d.stencil != Stencil::None && d.h.IsValid() && d.h.id < m_textures.size() &&
                    m_textures[d.h.id].planar)
                    check(node, d.stencil == Stencil::Write, d.stencilMask);
            for (RGTextureHandle h : node.stencilSrvHandles)
                if (h.IsValid() && h.id < m_textures.size() && m_textures[h.id].planar)
                    check(node, false, 0xFF);
        }
        std::string census;
        for (size_t f = 0; f < kFieldCount; ++f)
            census += std::string(StencilBits::kFields[f].name) + ":" + writers[f] + "/" + readers[f] + ";";
        census += clearers;
        for (const Outside& o : outside)
            census += std::string(";") + o.pass + (o.write ? "+w" : "+r") + std::to_string(o.bits);
        if (census == m_stencilCensus) return;
        m_stencilCensus = census;
        if (!declarations) return;
        for (size_t f = 0; f < kFieldCount; ++f)
            LOG_INFO("[RG_STENCIL] field=%s bits=0x%02X writers=%s readers=%s", StencilBits::kFields[f].name,
                     static_cast<uint32_t>(StencilBits::kFields[f].mask),
                     writers[f].empty() ? "-" : writers[f].c_str(), readers[f].empty() ? "-" : readers[f].c_str());
        LOG_INFO("[RG_STENCIL] clear=%s declarations=%u outside=%zu", clearers.empty() ? "-" : clearers.c_str(),
                 declarations, outside.size());
        for (const Outside& o : outside)
        {
            if (s_verify)
                ENGINE_CHECK(false, "[RG_STENCIL] owner pass=%s %s bits=0x%02X outside its fields 0x%02X "
                             "(-rgdagverify: see RenderGraph/StencilBits.h)", o.pass, o.write ? "writes" : "tests",
                             o.bits, o.allowed);
            else
                LOG_INFO("[RG_STENCIL] owner pass=%s %s bits=0x%02X outside its fields 0x%02X", o.pass,
                         o.write ? "writes" : "tests", o.bits, o.allowed);
        }
    }

    // ---- This frame's DAG and what its checks found ----
    // Serial, before Phase A, main graph only. Each (check, pass, texture) is
    // logged ONCE per Compile - the list stays finite and actionable, the way
    // the undeclared-access validator keeps its own - as INFO, or, for the
    // checks that are bugs (StaleRead, ConflictInPass, ClearAfterLive,
    // StencilOverlap), as an ERROR under -rgdagverify so a -maxerrors=0 run
    // fails on a new one.
    void RenderGraph::DeriveFrameDag()
    {
        const auto t0 = std::chrono::steady_clock::now();
        DeriveDag(m_passAccesses, m_activePasses, m_textures.size(), m_dag, m_dagScratch, &m_importFresh, &m_planar);
        m_dagUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();

        static const bool s_verify = Sys::CommandLine::Get().Has("-rgdagverify");
        for (const GraphSnapshot::Finding& f : m_dag.findings)
        {
            const uint64_t key = (static_cast<uint64_t>(f.check) << 56) |
                                 (static_cast<uint64_t>(f.pass & 0xFFFFFFu) << 32) | f.texture;
            if (std::find(m_dagReported.begin(), m_dagReported.end(), key) != m_dagReported.end())
                continue;
            m_dagReported.push_back(key);

            const char* check = GraphSnapshot::CheckName(f.check);
            const char* pass  = f.pass < m_passes.size() ? m_passes[f.pass].pass->InstanceName() : "?";
            const char* tex   = f.texture < m_textures.size() ? m_textures[f.texture].name.c_str() : "-";
            const char* after = f.other < m_passes.size() ? m_passes[f.other].pass->InstanceName() : "-";
            const bool bug = f.check == GraphSnapshot::Check::StaleRead ||
                             f.check == GraphSnapshot::Check::ConflictInPass ||
                             f.check == GraphSnapshot::Check::ClearAfterLive ||
                             f.check == GraphSnapshot::Check::StencilOverlap;
            if (bug && s_verify)
                ENGINE_CHECK(false, "[RG_DAG] finding check=%s pass=%s texture=%s after=%s "
                             "(-rgdagverify: see RenderGraph/RenderGraphDag.h)", check, pass, tex, after);
            else
                LOG_INFO("[RG_DAG] finding check=%s pass=%s texture=%s after=%s", check, pass, tex, after);
        }
    }

    void RenderGraph::EmitBarriersBeforePass(IGraphicsDevice& gfx,
                                              RHI::CommandList  cmd,
                                              PassNode&         node,
                                              uint32_t          passIndex,
                                              std::vector<GraphSnapshot::Barrier>* capture,
                                              std::vector<uint32_t>* uavCapture)
    {
        // Collected, then issued as ONE PushBarriers call: a pass commonly
        // transitions several textures at once (GBuffer's 6 MRTs + depth), and this
        // runs for every pass every frame. Order is preserved exactly: rt, then ds,
        // then uav, then srv.
        auto& batch = m_barrierScratch;
        batch.clear();
        batch.reserve(node.rtHandles.size()  + node.dsHandles.size()
                    + node.uavHandles.size() + node.srvHandles.size()
                    + node.scopedHandles.size() + node.manualHandles.size());

        // ---- Depth-state tripwire ----
        // The graph's tracker (vt.currentState) and the BACKEND's tracker are two
        // independent books on the same resource, and every barrier the graph emits
        // is written from the graph's. They stay in step only if everyone who touches
        // a graph texture OUTSIDE Phase A restores what they found - the post-graph
        // Hi-Z / SSR / probe-debug / compute chain, and the ManualBarrier passes.
        // When one does not, the D3D12 debug layer reports it as one line a minute
        // later naming only a command-list number, and only when the layer is on.
        // Ask the backend HERE instead: depth handles only (the out-of-graph round
        // trips are all on depth), once per handle per pass, ONCE per call site per
        // run. Never asserts; a divergence is a logged ERROR that names the pass and
        // both states in the debug layer's own hex. A depth-stencil texture's two
        // planes are each checked against their own book.
        auto tripwire = [&](RGTextureHandle h)
        {
            if (!h.IsValid() || h.id >= m_textures.size()) return;
            auto& vt = m_textures[h.id];
            // An import has its own tripwire below (a pass's depth import; an
            // Owner-tracked one has none).
            if (!vt.desc.isDepth || !vt.texture.IsValid() || vt.imported) return;
            for (int plane = 0; plane < (vt.planar ? 2 : 1); ++plane)
            {
                uint32_t deviceState = 0, graphState = 0;
                const RHI::ResourceState mine = plane == 1 ? vt.stencilState : vt.currentState;
                ENGINE_CHECK_ONCE(gfx.DeviceStateAgrees(vt.texture, mine, &deviceState, &graphState,
                                                        vt.planar ? plane : -1),
                                  "RenderGraph depth tracker divergence before pass '%s' on '%s'%s: "
                                  "graph says 0x%X, device tracker says 0x%X. Something transitioned "
                                  "this depth texture outside the graph and did not restore it - see "
                                  "the post-graph depth chain in Renderer::Render.",
                                  node.pass ? node.pass->InstanceName() : "<null>",
                                  vt.name.c_str(), vt.planar ? (plane == 1 ? " (stencil plane)" : " (depth plane)") : "",
                                  static_cast<unsigned>(graphState),
                                  static_cast<unsigned>(deviceState));
            }
        };
        for (auto& h : node.dsHandles)     tripwire(h);
        for (auto& h : node.srvHandles)    tripwire(h);
        for (auto& h : node.manualHandles) tripwire(h);
        for (const PassNode::DsvDecl& d : node.dsvDecls) tripwire(d.h);
        for (auto& h : node.stencilSrvHandles) tripwire(h);
        for (const auto& c : node.copySrc) tripwire(c.first);
        for (const auto& c : node.copyDst) tripwire(c.first);

        // ---- The import tripwire ----
        // An import's tracker starts each Execute from the device's book, and the
        // graph's barriers move both books together, so the two disagree only if
        // something changed the device's state since, outside the graph, without
        // restoring it. Checked before every pass that declares a Graph-tracked
        // import (any kind of declaration), once per run, in the same hex; an
        // Owner-tracked one's book belongs to its owner.
        auto importTripwire = [&](RGTextureHandle h)
        {
            if (!h.IsValid() || h.id >= m_textures.size()) return;
            auto& vt = m_textures[h.id];
            if (!vt.imported || !vt.texture.IsValid() || vt.OwnerTracked()) return;
            uint32_t deviceState = 0, graphState = 0;
            ENGINE_CHECK_ONCE(gfx.DeviceStateAgrees(vt.texture, vt.currentState, &deviceState, &graphState),
                              "RenderGraph import tracker divergence before pass '%s' on '%s': graph says "
                              "0x%X, device tracker says 0x%X. Something transitioned the imported texture "
                              "outside the graph since this Execute began and did not restore it.",
                              node.pass ? node.pass->InstanceName() : "<null>", vt.name.c_str(),
                              static_cast<unsigned>(graphState), static_cast<unsigned>(deviceState));
        };
        for (auto& h : node.rtHandles)     importTripwire(h);
        for (auto& h : node.dsHandles)     importTripwire(h);
        for (auto& h : node.uavHandles)    importTripwire(h);
        for (auto& h : node.srvHandles)    importTripwire(h);
        for (auto& h : node.scopedHandles) importTripwire(h);
        for (auto& h : node.manualHandles) importTripwire(h);

        // A depth-stencil texture's planes move apart only through the plane
        // declarations; @p depthTarget / @p stencilTarget UNDEFINED = leave that
        // plane. Planes in one state going to one target take ONE barrier on every
        // plane, the same barrier a whole-texture declaration gets.
        auto transitionPlanes = [&](RGTextureHandle h, RHI::ResourceState depthTarget,
                                    RHI::ResourceState stencilTarget)
        {
            if (!h.IsValid() || h.id >= m_textures.size()) return;
            auto& vt = m_textures[h.id];
            if (!vt.texture.IsValid()) return;
            if (vt.OwnerTracked()) return;   // its owner transitions it
            const RHI::ResourceState d = vt.currentState;
            const RHI::ResourceState s = vt.planar ? vt.stencilState : d;
            const bool moveD = depthTarget != RHI::ResourceState::UNDEFINED && d != depthTarget;
            const bool moveS = vt.planar && stencilTarget != RHI::ResourceState::UNDEFINED && s != stencilTarget;
            if (!vt.planar || (moveD && moveS && d == s && depthTarget == stencilTarget))
            {
                if (!moveD) return;
                batch.push_back(RHI::GPUBarrier::Image(&vt.texture, d, depthTarget));
                if (capture)   // a snapshot frame
                    capture->push_back({ h.id, static_cast<uint32_t>(d), static_cast<uint32_t>(depthTarget) });
                vt.currentState = depthTarget;
                if (vt.planar) vt.stencilState = depthTarget;
                return;
            }
            if (moveD)
            {
                batch.push_back(RHI::GPUBarrier::ImagePlane(&vt.texture, 0, d, depthTarget));
                if (capture)
                    capture->push_back({ h.id, static_cast<uint32_t>(d), static_cast<uint32_t>(depthTarget), 0 });
                vt.currentState = depthTarget;
            }
            if (moveS)
            {
                batch.push_back(RHI::GPUBarrier::ImagePlane(&vt.texture, 1, s, stencilTarget));
                if (capture)
                    capture->push_back({ h.id, static_cast<uint32_t>(s), static_cast<uint32_t>(stencilTarget), 1 });
                vt.stencilState = stencilTarget;
            }
        };
        // The whole texture (every plane of a depth-stencil one).
        auto transition = [&](RGTextureHandle h, RHI::ResourceState targetState)
        {
            transitionPlanes(h, targetState, targetState);
        };
        // A plane a DSV binding uses only for the test, or not at all: where it
        // is, when that state takes a DSV (DEPTH_WRITE, or a DEPTH_READ state),
        // else DEPTH_READ_SRV - the state the later shader reads also take.
        auto dsvRead = [](RHI::ResourceState s)
        {
            return s == RHI::ResourceState::DEPTHSTENCIL || s == RHI::ResourceState::DEPTH_READ_SRV ||
                   s == RHI::ResourceState::DEPTHSTENCIL_READONLY;
        };
        const bool planes = PlanesEnabled();

        // ---- UAV after UAV ----
        // Two passes in a row using a texture as UAV get NO transition between them
        // (the state is already UNORDERED_ACCESS), and EndFrame batches same-queue
        // lists into ONE ExecuteCommandLists, inside which D3D12 orders nothing: the
        // second could read or overwrite what the first is still writing. A UAV
        // barrier before the second closes that. Any other access in between
        // transitions the texture, which already orders it, so the tracker forgets
        // the UAV user (m_uavLastPass, reset per Execute). -rguavbarrieroff: the
        // escape.
        static const bool s_uavBarrierOff = Sys::CommandLine::Get().Has("-rguavbarrieroff");
        auto forgetUav = [&](RGTextureHandle h)
        {
            if (h.IsValid() && h.id < m_uavLastPass.size()) m_uavLastPass[h.id] = -1;
        };
        auto uavAccess = [&](RGTextureHandle h)
        {
            if (!h.IsValid() || h.id >= m_textures.size() || h.id >= m_uavLastPass.size()) return;
            auto& vt = m_textures[h.id];
            if (vt.OwnerTracked()) return;   // its owner barriers it
            int32_t& last = m_uavLastPass[h.id];
            if (!s_uavBarrierOff && vt.texture.IsValid() && last >= 0 &&
                last != static_cast<int32_t>(passIndex) &&
                vt.currentState == RHI::ResourceState::UNORDERED_ACCESS)
            {
                batch.push_back(RHI::GPUBarrier::Memory(&vt.texture));
                if (uavCapture) uavCapture->push_back(h.id);
            }
            last = static_cast<int32_t>(passIndex);
        };

        for (auto& h : node.rtHandles)  { forgetUav(h); transition(h, RHI::ResourceState::RENDERTARGET); }
        for (auto& h : node.dsHandles)  { forgetUav(h); transition(h, RHI::ResourceState::DEPTHSTENCIL); }
        // The DepthStencil declarations - each plane by what the pass does to it
        // (RG::Depth / RG::Stencil). -rgplaneoff: a DSV the pass may write, every
        // plane in DEPTH_WRITE, as WriteDepthStencil.
        for (const PassNode::DsvDecl& d : node.dsvDecls)
        {
            forgetUav(d.h);
            if (!planes) { transition(d.h, RHI::ResourceState::DEPTHSTENCIL); continue; }
            if (!d.h.IsValid() || d.h.id >= m_textures.size()) continue;
            const VirtualTexture& vt = m_textures[d.h.id];
            const RHI::ResourceState ds = RHI::ResourceState::DEPTHSTENCIL;
            const RHI::ResourceState rd = RHI::ResourceState::DEPTH_READ_SRV;
            const RHI::ResourceState keep = RHI::ResourceState::UNDEFINED;
            const RHI::ResourceState dt = d.depth == Depth::Write ? ds : (dsvRead(vt.currentState) ? keep : rd);
            const RHI::ResourceState st = !vt.planar ? keep
                                        : d.stencil == Stencil::Write ? ds : (dsvRead(vt.stencilState) ? keep : rd);
            transitionPlanes(d.h, dt, st);
        }
        // The copies: one plane of a depth-stencil texture, the whole of any other.
        for (bool dst : { true, false })
            for (const auto& c : dst ? node.copyDst : node.copySrc)
            {
                forgetUav(c.first);
                const RHI::ResourceState to = dst ? RHI::ResourceState::COPY_DST : RHI::ResourceState::COPY_SRC;
                const bool onePlane = planes && c.first.IsValid() && c.first.id < m_textures.size() &&
                                      m_textures[c.first.id].planar;
                if (!onePlane)                       transition(c.first, to);
                else if (c.second == Plane::Stencil) transitionPlanes(c.first, RHI::ResourceState::UNDEFINED, to);
                else                                 transitionPlanes(c.first, to, RHI::ResourceState::UNDEFINED);
            }
        for (auto& h : node.uavHandles)
        {
            uavAccess(h);   // reads the state BEFORE this pass's transition
            transition(h, RHI::ResourceState::UNORDERED_ACCESS);
        }
        for (auto& h : node.srvHandles)
        {
            forgetUav(h);
            const bool isDepthTex = (h.id < m_textures.size()) && m_textures[h.id].desc.isDepth;
            if (!isDepthTex)
                transition(h, RHI::ResourceState::SHADER_RESOURCE);
            // A depth-stencil texture's SRV is its depth plane's; the stencil plane
            // stays where it is.
            else if (planes && m_textures[h.id].planar)
                transitionPlanes(h, RHI::ResourceState::DEPTH_READ_SRV, RHI::ResourceState::UNDEFINED);
            else
                transition(h, RHI::ResourceState::DEPTH_READ_SRV);
        }
        // A stencil plane's SRV (DEPTH_READ_SRV holds both shader-read states); the
        // whole texture under -rgplaneoff.
        for (auto& h : node.stencilSrvHandles)
        {
            forgetUav(h);
            if (planes) transitionPlanes(h, RHI::ResourceState::UNDEFINED, RHI::ResourceState::DEPTH_READ_SRV);
            else        transition(h, RHI::ResourceState::DEPTH_READ_SRV);
        }
        // Scoped reads: brought into the state the pass reads them at; the pass's
        // own list leaves them there, so the tracker stays.
        for (size_t k = 0; k < node.scopedHandles.size(); ++k)
        {
            forgetUav(node.scopedHandles[k]);
            if (k < node.scopedStates.size())
                transition(node.scopedHandles[k], node.scopedStates[k]);
        }
        // Manual-barrier handles that DECLARED an entry state. Last, so a handle
        // that is somehow in two lists ends in the state its own pass asked for.
        // UNDEFINED entries (the default) transition nothing.
        for (size_t i = 0; i < node.manualHandles.size(); ++i)
        {
            forgetUav(node.manualHandles[i]);   // it barriers itself: no UAV user to trust
            if (i >= node.manualEntryStates.size()) continue;
            const RHI::ResourceState want = node.manualEntryStates[i];
            if (want == RHI::ResourceState::UNDEFINED) continue;
            transition(node.manualHandles[i], want);
        }

        // Each DepthStencil declaration's DSV view, from the planes' states this pass
        // records in - a plane in DEPTH_WRITE binds writable, one in a DEPTH_READ
        // state read-only - and what the pass declared it writes (the device's
        // draw-time depth tripwire). Read by RenderContext::DepthView while the pass
        // records; none under -rgplaneoff (the writable DSV, nothing checked).
        node.depthViews.clear();
#if WITH_HARNESS
        // -rgtest_depthlie=<pass> (harness): the draw-time depth tripwire's reverse
        // arm - that pass's views say it writes no plane, so its first draw with a
        // pipeline that writes depth or stencil must be reported.
        // -rgtest_stencillie=<pass>: its views name no stencil bit while still
        // letting it write the plane - the stencil-bit half.
        static const std::string s_depthLie = Sys::CommandLine::Get().Get("-rgtest_depthlie");
        static const std::string s_stencilLie = Sys::CommandLine::Get().Get("-rgtest_stencillie");
        const bool depthLie = !s_depthLie.empty() && node.pass && s_depthLie == node.pass->InstanceName();
        const bool stencilLie = !s_stencilLie.empty() && node.pass && s_stencilLie == node.pass->InstanceName();
#else
        const bool depthLie = false, stencilLie = false;
#endif
        if (planes)
            for (const PassNode::DsvDecl& d : node.dsvDecls)
            {
                if (!d.h.IsValid() || d.h.id >= m_textures.size()) continue;
                const VirtualTexture& vt = m_textures[d.h.id];
                RHI::DepthView v;
                v.readOnlyDepth   = vt.currentState != RHI::ResourceState::DEPTHSTENCIL;
                v.readOnlyStencil = vt.planar && vt.stencilState != RHI::ResourceState::DEPTHSTENCIL;
                v.mayWriteDepth   = d.depth == Depth::Write && !depthLie;
                v.mayWriteStencil = d.stencil == Stencil::Write && !depthLie;
                // The bits it declared (RenderGraph/StencilBits.h).
                v.stencilWriteMask = d.stencil == Stencil::Write ? (stencilLie ? uint8_t(0) : d.stencilMask)
                                                                 : uint8_t(0xFF);
                node.depthViews.emplace_back(d.h.id, v);
            }

        if (!batch.empty())
            gfx.PushBarriers(batch.data(), static_cast<uint32_t>(batch.size()), cmd);
    }

} // namespace RG
