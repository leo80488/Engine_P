// MaterialEditorWindow.cpp - see the header.
#include "Editor/MaterialEditorWindow.h"

#include "ECS/GuidComponent.h"          // an entity tab finds its entity again
#include "ECS/GuidRegistry.h"
#include "ECS/HierarchyComponents.h"    // MaterialSourcePath, MaterialOverride

#include "Editor/EditorLayerShared.h"      // SaveFileDialog
#include "Editor/EditorUITests.h"          // the tab x's reverse arm
#include "Editor/MaterialPreviewWidget.h"
#include "Editor/MeshEditorWindow.h"       // the view modes' names
#include "Graphics/PermutationKey.h"       // the permutations a live shader is compiled for
#include "Graphics/ShaderHotReload.h"      // RequestShaderReload (a live shader no watcher sees)
#include "Graphics/ShaderLibrary.h"        // CompileForCache
#include "Graphics/MaterialSurfaceCache.h" // where rays get a custom material's surface
#include "Graphics/GraphicsDX12.h"         // a node thumbnail's preview SRV
#include "Resource/TextureSystem.h"        // node thumbnails stream as the Content Browser's tiles
#include "Harness/ControlChannelJobs.h"
#include "MaterialGraph/MaterialGraphNodes.h"   // Create Graph wires the Output's pins
#include "Renderer/Renderer.h"
#include "Resource/AssetGuid.h"            // a graph's name carries its material's guid
#include "Resource/AssetRefs.h"            // PercentDecode (-mateditor_* paths)
#include "Resource/MaterialLiveOverlay.h"  // the live overlay
#include "Resource/MaterialSerializer.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/Sha256.h"                 // a switch combination's file name

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW: "Open" a custom shader's .hlsl

namespace Editor
{
namespace
{
    constexpr std::uint64_t kDiskCheckFrames = 30;   // how often the tabs' files are stat'ed

    enum StepKind { kStepSet = 1, kStepOverride, kStepView, kStepSave, kStepSaveInstance, kStepReopen,
                    kStepDump, kStepUndo, kStepRedo, kStepRevert, kStepClose, kStepAssign, kStepEntity,
                    kStepActivate, kStepGraphCreate, kStepGraphOps, kStepGraphView, kStepGraphPreview };

    constexpr int kMaxSwitchCombinations = 16;   // per graph: each file is a permanent registry entry

    constexpr double kLiveDebounceSeconds = 0.25;   // edits settle this long before a live compile

    // RefreshCombinations' tally: this graph's other combination files, the ones whose bytes
    // changed and were written, the ones that did not compile or could not be written.
    struct CombinationRefresh { int files = 0, rewritten = 0, failed = 0; };

    // Where a material's graph and generated HLSL live: a project material's in
    // asset/Shaders/Generated (watched, packed as .hlsl); one outside asset/ (a harness copy)
    // keeps them beside it, never in the tree.
    std::string GeneratedDirFor(const std::string& materialPath)
    {
        if (materialPath.rfind("asset/", 0) == 0) return "asset/Shaders/Generated";
        const std::size_t s = materialPath.find_last_of('/');
        return (s == std::string::npos ? std::string(".") : materialPath.substr(0, s)) + "/Generated";
    }

    std::string StemOf(const std::string& path)
    {
        std::string b = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
        const std::size_t dot = b.find_last_of('.');
        return dot == std::string::npos ? b : b.substr(0, dot);
    }

    // x.imatgraph -> x.hlsl: the generated shader sits beside its graph.
    std::string ShaderPathForGraph(const std::string& graph)
    {
        const std::size_t dot = graph.find_last_of('.');
        return (dot == std::string::npos ? graph : graph.substr(0, dot)) + ".hlsl";
    }

    // The material's part of a combination's key (ShaderPathForCombination hashes it after the
    // switches). A graph with an owner (one material's) compiles with that material's shading
    // model and blend mode, so its files are named by the switches alone ("" here). A graph
    // WITHOUT one - an imported master its instances' materials name
    // (tools/cartoonstreet_migration/ue_graph_import.py computes the same names) - adds the
    // material's: the compile depends on them, and two materials must never write one file
    // with different bytes.
    std::string MaterialKey(const MaterialGraph::Graph& g, const MaterialComponent& m)
    {
        if (!g.ownerGuid.empty() || g.function) return {};
        return "shading=" + std::to_string(static_cast<int>(m.customShadingModel)) + ";blend=" +
               std::to_string(static_cast<std::uint32_t>(m.userBlendMode)) + ";";
    }

    // One shader file per Static Switch combination: x.hlsl when every switch is at its
    // default and @p materialKey is empty (a graph without switches keeps its one file), else
    // x_<sha8 of the name=value list, then @p materialKey>.hlsl (@p switches come sorted by name).
    std::string ShaderPathForCombination(const std::string& graph, const std::vector<MaterialGraph::StaticSwitchInfo>& switches,
                                         const std::string& materialKey)
    {
        std::string key;
        bool defaults = materialKey.empty();
        for (const MaterialGraph::StaticSwitchInfo& w : switches)
        {
            key += w.name + "=" + (w.value ? "1" : "0") + ";";
            defaults = defaults && w.value == w.def;
        }
        key += materialKey;
        const std::string base = ShaderPathForGraph(graph);
        if (defaults) return base;
        return base.substr(0, base.size() - 5) + "_" + Sys::Sha256Hex(key.data(), key.size()).substr(0, 8) + ".hlsl";
    }

    // The combination files a graph has now (x_<8 hex>.hlsl beside it).
    int CombinationFiles(const std::string& graph)
    {
        const std::string base = ShaderPathForGraph(graph);
        const std::filesystem::path dir = Sys::U8Path(base).parent_path();
        const std::string stem = Sys::PathToUtf8(Sys::U8Path(base).stem()) + "_";
        int count = 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = Sys::PathToUtf8(it->path().filename());
            if (name.size() == stem.size() + 8 + 5 && name.rfind(stem, 0) == 0 && name.compare(name.size() - 5, 5, ".hlsl") == 0)
                ++count;
        }
        return count;
    }

    std::string WithoutCR(std::string s)
    {
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        return s;
    }

    // A graph's live shader: ONE fixed file per graph, <Generated>/.edit/<graph>.hlsl (every
    // other path would be another permanent DynamicShaderRegistry entry). ".edit" is gitignored
    // and never packaged.
    std::string LiveShaderPathFor(const std::string& materialPath, const std::string& graphPath)
    {
        return GeneratedDirFor(materialPath) + "/.edit/" + StemOf(graphPath) + ".hlsl";
    }

    // The permutations the first draws of a material ask for: Renderer::ApplyBlendMode's
    // bits (DOUBLE_SIDED, the blend bits, ALPHA_TEST, WATER_SURFACE) on a static mesh. A miss
    // costs only that draw one synchronous compile. The pixel stage always, the vertex stage
    // when World Position Offset is linked.
    std::vector<std::pair<RHI::ShaderStage, std::uint32_t>> LivePermutations(const MaterialComponent& m, bool vertex)
    {
        std::uint32_t bits = 0;
        if (m.IsDoubleSided()) bits |= PermutationKey::DOUBLE_SIDED;
        switch (m.userBlendMode)
        {
            case BlendMode::Alpha:
                bits |= PermutationKey::ALPHA_BLEND;
                if (m.IsAlphaTestEnabled()) bits |= PermutationKey::ALPHA_TEST;
                break;
            case BlendMode::Additive:      bits |= PermutationKey::ADDITIVE_BLEND; break;
            case BlendMode::Premultiplied:
                bits |= PermutationKey::PREMULTIPLIED_BLEND;
                if (m.IsAlphaTestEnabled()) bits |= PermutationKey::ALPHA_TEST;
                break;
            case BlendMode::Multiply:      bits |= PermutationKey::MULTIPLY_BLEND; break;
            case BlendMode::Transmissive:
                bits |= PermutationKey::TRANSMISSIVE_BLEND;
                if (m.IsAlphaTestEnabled()) bits |= PermutationKey::ALPHA_TEST;
                break;
            case BlendMode::Masked:
            case BlendMode::Dithered:      bits |= PermutationKey::ALPHA_TEST; break;
            default:                       if (m.IsAlphaTestEnabled()) bits |= PermutationKey::ALPHA_TEST; break;
        }
        if (m.IsWaterShadingModel()) bits = (bits | PermutationKey::WATER_SURFACE) & ~PermutationKey::ALPHA_TEST;
        std::vector<std::pair<RHI::ShaderStage, std::uint32_t>> out{ { RHI::ShaderStage::PS, bits } };
        if (vertex) out.push_back({ RHI::ShaderStage::VS, bits });
        return out;
    }

    // Under a tree one of App's shader watchers opened (shaders/, asset/Shaders/ if it existed
    // at boot): a change there hot-reloads by itself; anywhere else it has to be requested.
    bool Watched(const std::string& path)
    {
        return HotReload::IsWatchedShaderPath(path);
    }

    double SteadySeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string o;
        for (const char c : s)
        {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
            else o += c;
        }
        return o;
    }

    std::string BaseName(const std::string& p)
    {
        const std::size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string Canon(const std::string& path)
    {
        std::string p = Sys::RelativizeAssetPath(path);
        for (char& c : p) if (c == '\\') c = '/';
        return p;
    }

    // A graph's OTHER combinations (every file but @p written), recompiled from @p graph (just
    // saved at @p graphPath): every material naming it - each with its own switches, shading
    // model and blend mode - gets the edit, as an Unreal master's instances do. A file's
    // combination is its own header (MaterialGraph::ParseGeneratedHeader); a file that is not
    // this graph's generated shader is left alone, one whose bytes do not change is not written.
    CombinationRefresh RefreshCombinations(const MaterialGraph::Graph& graph, const std::string& graphPath,
                                           const std::string& written)
    {
        CombinationRefresh out;
        const std::string base = ShaderPathForGraph(graphPath);          // <dir>/<stem>.hlsl
        const std::size_t slash = base.find_last_of('/');
        const std::string prefix = slash == std::string::npos ? std::string() : base.substr(0, slash + 1);
        const std::string stem = base.substr(prefix.size(), base.size() - prefix.size() - 5);
        std::vector<std::string> files;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(Sys::U8Path(prefix.empty() ? std::string(".") : prefix), ec), end;
             !ec && it != end; it.increment(ec))
        {
            const std::string name = Sys::PathToUtf8(it->path().filename());
            const bool combination = name == stem + ".hlsl" ||
                (name.size() == stem.size() + 14 && name.rfind(stem + "_", 0) == 0 &&
                 name.compare(name.size() - 5, 5, ".hlsl") == 0 &&
                 std::all_of(name.begin() + static_cast<std::ptrdiff_t>(stem.size() + 1), name.end() - 5,
                             [](unsigned char c) { return std::isxdigit(c) != 0; }));
            if (combination && prefix + name != written) files.push_back(prefix + name);
        }
        std::sort(files.begin(), files.end());
        for (const std::string& f : files)
        {
            std::string text, from;
            MaterialGraph::CompileOptions o;
            if (!Sys::ReadFileText(f, text) || !MaterialGraph::ParseGeneratedHeader(text, from, o) || Canon(from) != Canon(graphPath))
                continue;
            ++out.files;
            o.sourceLabel = graphPath;
            const MaterialGraph::CompileResult r = MaterialGraph::Compile(graph, o);
            if (!r.ok)
            {
                // Its old shader stays: this combination's material shows the old look until fixed.
                ++out.failed;
                const MaterialGraph::Diagnostic* e = nullptr;
                for (const MaterialGraph::Diagnostic& d : r.diagnostics)
                    if (!e && d.severity == MaterialGraph::Severity::Error) e = &d;
                LOG_WARNING("[MATGRAPH] combination %s: does not compile (%s) - kept", f.c_str(),
                            e ? e->message.c_str() : "?");
                continue;
            }
            if (WithoutCR(text) == r.hlsl) continue;
            if (!Sys::WriteFileBytesAtomic(f, r.hlsl.data(), r.hlsl.size()))
            {
                ++out.failed;
                LOG_WARNING("[MATGRAPH] combination %s: cannot write it", f.c_str());
                continue;
            }
            if (!Watched(f)) HotReload::RequestShaderReload(f);
            ++out.rewritten;
        }
        return out;
    }

    // Where rays get this material's surface (they never run a custom shader), for Stats.
    // Mirrors the renderer's surface-cache page gate (Renderer::SurfaceAtlasEligible).
    std::string RtProxySource(const MaterialComponent& m, const Renderer* renderer)
    {
        if (!m.useCustomShader || m.customShaderPath.empty()) return "engine slots (a standard material)";
        if (m.rtUseStandardMaterial) return "engine slots (rtUseStandardMaterial)";
        const MaterialSurfaceCache* sc = renderer ? renderer->GetMaterialSurfaceCache() : nullptr;
        const bool identityUv = m.texMulAdd.x == 1.0f && m.texMulAdd.y == 1.0f && m.texMulAdd.z == 0.0f && m.texMulAdd.w == 0.0f;
        if (sc && sc->IsEnabled() && m.userBlendMode == BlendMode::Opaque && !m.IsAlphaTestEnabled() &&
            !m.IsWaterShadingModel() && !m.HasPerFrameParams() && identityUv)
            return "its own shader, baked (surface cache)";
        return "parameter names (TintColor, EmissiveColor, AlbedoMap), else the engine slots";
    }

    // Shadows clip an alpha-tested material by its ENGINE slots only (Shadow.ps:
    // BASECOLORMAP.a x OPACITYMAP.r). A custom material whose slots hold no mask (OPACITYMAP
    // empty, the default base colour) casts the whole polygon, whatever its shader makes of it
    // (for a graph: when it reads an Opacity Mask / Opacity at all).
    bool ShadowMaskGap(const MaterialComponent& m, const MaterialGraphDocument* graph)
    {
        if (!m.useCustomShader || m.customShaderPath.empty() || !m.IsAlphaTestEnabled()) return false;
        const std::string& base = m.textures[MaterialComponent::BASECOLORMAP].name;
        if (!m.textures[MaterialComponent::OPACITYMAP].name.empty() ||
            !(base.empty() || base == "engine/textures/Default_BaseColor.itex"))
            return false;
        if (!graph) return true;   // a hand-written shader may compute one
        const MaterialGraph::Graph& g = graph->Graph();
        for (const MaterialGraph::Node& n : g.nodes)
            if (n.type == "MaterialOutput")
                for (const char* pin : { "OpacityMask", "Opacity" })
                    if (g.FindInputLink(n.id, pin) && MaterialGraph::OutputPinActive(pin, m.customShadingModel, m.userBlendMode))
                        return true;
        return false;
    }

    // The texture a graph's Opacity Mask reads (its R), as the material binds it: what
    // "Mirror mask" puts into OPACITYMAP. "" when the mask is not one texture's R channel.
    std::string MirrorMaskPath(const MaterialGraph::CompileResult& r, const MaterialComponent& m)
    {
        if (r.opacityMaskTexture < 0 || r.opacityMaskTexture >= static_cast<int>(r.textures.size()) ||
            r.opacityMaskChannel != "R")
            return {};
        const MaterialGraph::TextureInfo& t = r.textures[static_cast<std::size_t>(r.opacityMaskTexture)];
        if (t.parameter)
        {
            const auto it = m.customTextures.find(t.hlslName);
            if (it != m.customTextures.end() && !it->second.name.empty()) return it->second.name;
        }
        return t.defaultPath;
    }

    // "<value>@<frame>" -> value, frame. No '@' = frame 0.
    bool SplitAt(const std::string& item, std::string& value, std::uint64_t& at)
    {
        const std::size_t p = item.find_last_of('@');
        if (p == std::string::npos) { value = item; at = 0; return true; }
        value = item.substr(0, p);
        const std::string f = item.substr(p + 1);
        if (f.empty() || !std::all_of(f.begin(), f.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
            return false;
        at = std::strtoull(f.c_str(), nullptr, 10);
        return true;
    }

    std::vector<std::string> SplitList(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        std::size_t b = 0;
        while (b <= s.size())
        {
            const std::size_t e = s.find(sep, b);
            std::string item = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
            if (!item.empty()) out.push_back(std::move(item));
            if (e == std::string::npos) break;
            b = e + 1;
        }
        return out;
    }

    bool IsFrame(const std::string& s)
    {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    }

    // A harness file name: relative = under the run's diagnostics folder.
    std::string HarnessPath(const std::string& p)
    {
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\');
        return absolute ? p : Sys::DiagnosticsDir() + "/" + p;
    }

    std::string JoinSorted(const std::vector<std::string>& v)
    {
        std::string s;
        for (const std::string& k : v) s += (s.empty() ? "" : ",") + k;
        return s.empty() ? "-" : s;
    }

    std::vector<std::string> Sorted(const std::unordered_set<std::string>& set)
    {
        std::vector<std::string> v(set.begin(), set.end());
        std::sort(v.begin(), v.end());
        return v;
    }

    // What an entity overrides (its MaterialOverride keys), sorted, and its asset.
    std::vector<std::string> OverrideKeysOf(World& w, Entity e)
    {
        std::vector<std::string> keys;
        if (const MaterialOverride* o = w.GetComponent<MaterialOverride>(e))
            for (const auto& [k, v] : o->props) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        return keys;
    }
    std::string SourceOf(World& w, Entity e)
    {
        const MaterialSourcePath* src = w.GetComponent<MaterialSourcePath>(e);
        return src ? src->path : std::string();
    }

    // "key=value;key=value" -> pairs (the first '=' splits; values may hold spaces).
    bool ParseAssignments(const std::string& text, std::vector<std::pair<std::string, std::string>>& out)
    {
        for (const std::string& kv : SplitList(text, ';'))
        {
            const std::size_t e = kv.find('=');
            if (e == std::string::npos || e == 0) return false;
            out.emplace_back(kv.substr(0, e), kv.substr(e + 1));
        }
        return !out.empty();
    }
}

MaterialEditorWindow& MaterialEditorWindow::Get()
{
    static MaterialEditorWindow s_instance;
    return s_instance;
}

MaterialEditorWindow::MaterialEditorWindow() : m_preview(std::make_unique<MaterialPreviewWidget>())
{
    m_preview->SetShowViewModeControl(true);
}

MaterialEditorWindow::~MaterialEditorWindow() = default;

// ==== Tabs ====
MaterialEditorWindow::Tab* MaterialEditorWindow::ActiveTabPtr()
{
    return (m_active >= 0 && m_active < static_cast<int>(m_tabs.size())) ? &m_tabs[m_active] : nullptr;
}

MaterialEditSession* MaterialEditorWindow::ActiveSession()
{
    Tab* t = ActiveTabPtr();
    return t ? t->session.get() : nullptr;
}

bool MaterialEditorWindow::TabDirty(const Tab& t)
{
    return t.session->Dirty() || (t.graph && t.graph->Dirty());
}

bool MaterialEditorWindow::AnyDirty() const
{
    return std::any_of(m_tabs.begin(), m_tabs.end(), [](const Tab& t) { return TabDirty(t); });
}

std::vector<std::string> MaterialEditorWindow::UnsavedFiles() const
{
    std::vector<std::string> out;
    for (const Tab& t : m_tabs)
        if (TabDirty(t)) out.push_back(t.session->Path().empty() ? t.name : t.session->Path());
    return out;
}

bool MaterialEditorWindow::SaveAllNow(const MaterialEditorContext& ctx, std::string& why)
{
    bool ok = true;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
    {
        if (!TabDirty(m_tabs[i])) continue;
        PendingSave p;
        p.tab = i;
        RunSave(ctx, p);
        // RunSave logs its own failure; still dirty is what tells this caller.
        if (i < static_cast<int>(m_tabs.size()) && TabDirty(m_tabs[i]) && ok)
        {
            why = "could not save '" + m_tabs[i].session->Path() + "'";
            ok = false;
        }
    }
    return ok;
}

void MaterialEditorWindow::RequestOpen(const std::string& path)
{
    m_open = true;
    m_pendingOpen.push_back(path);
}

void MaterialEditorWindow::RequestClose()
{
    if (AnyDirty()) { if (m_askFor == -1) { m_askFor = -2; m_askOpenPopup = true; } }
    else m_open = false;
}

void MaterialEditorWindow::LogOpen(const MaterialEditSession& s) const
{
    const MaterialComponent& m = s.Mat();
    // overrides / keys: an instance's set (a standalone file writes every key: "-").
    LOG_INFO("[MATEDITOR] open %s: kind=%s parent=%s overrides=%zu keys=%s explicit=%d params=%zu textures=%zu "
             "shader=%s tabs=%zu", s.Path().c_str(), MaterialKindName(s.Kind()),
             s.IsInstance() ? s.Info().parentPath.c_str() : "-", s.IsInstance() ? s.Info().overrideKeys.size() : 0,
             s.IsInstance() ? JoinSorted(Sorted(s.Info().overrideKeys)).c_str() : "-", s.Info().explicitFormat ? 1 : 0,
             m.customParams.size(), m.customTextures.size(),
             m.useCustomShader && !m.customShaderPath.empty() ? m.customShaderPath.c_str() : "-", m_tabs.size());
}

void MaterialEditorWindow::LogDump(const MaterialEditSession& s, const std::string& keys) const
{
    // Values as the file format spells them (BuildMaterialText's lines).
    const std::string text = Resource::BuildMaterialText(s.Mat());
    std::string out;
    for (const std::string& key : SplitList(keys, ';'))
    {
        std::istringstream in(text);
        std::string line, value = "<absent>";
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            if (k != key) continue;
            value = line.substr(eq + 1);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            break;
        }
        out += " " + key + "=" + value;
    }
    LOG_INFO("[MATEDITOR] dump %s:%s overridden=%s", s.Path().c_str(), out.c_str(),
             JoinSorted(Sorted(s.Info().overrideKeys)).c_str());
}

bool MaterialEditorWindow::OpenNow(const std::string& pathIn, std::string& why)
{
    const std::string path = Canon(pathIn);
    if (path.empty()) { why = "a path"; return false; }
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        if (Lower(m_tabs[i].session->Path()) == Lower(path))
        {
            m_open = true;
            m_active = m_focusTab = i;
            return true;
        }
    if (static_cast<int>(m_tabs.size()) >= kMaxTabs)
    {
        why = "already " + std::to_string(kMaxTabs) + " tabs open - close one first";
        return false;
    }
    std::string ext = Lower(std::filesystem::path(Sys::U8Path(path)).extension().string());
    if (ext == ".imatfunc")
    {
        // A Material Function: its graph, and a transient material to preview it with.
        auto g = std::make_unique<MaterialGraphDocument>();
        if (!g->Open(path, why)) return false;
        if (!g->IsFunction()) { why = "'" + path + "' is not a Material Function (its format is not imatfunc)"; return false; }
        auto session = std::make_unique<MaterialEditSession>();
        session->OpenFunction(path);
        Tab t;
        t.name    = BaseName(path);
        t.graph   = std::move(g);
        t.session = std::move(session);
        m_tabs.push_back(std::move(t));
        m_active = m_focusTab = static_cast<int>(m_tabs.size()) - 1;
        m_open = true;
        LogOpen(*m_tabs.back().session);
        return true;
    }
    if (ext != ".imat" && ext != ".imatinst")
    {
        why = "'" + path + "' is not a material (.imat / .imatinst) or a Material Function (.imatfunc)";
        return false;
    }
    auto session = std::make_unique<MaterialEditSession>();
    if (!session->Open(path, why)) return false;
    Tab t;
    t.name    = BaseName(session->Path());
    // A standalone material with a graph edits the graph (an instance of one edits its
    // parameters only: the graph is its parent's).
    if (!session->IsInstance() && !session->Mat().materialGraph.empty())
    {
        auto g = std::make_unique<MaterialGraphDocument>();
        if (g->Open(session->Mat().materialGraph, t.graphError)) t.graph = std::move(g);
        else LOG_WARNING("[MATGRAPH] %s: its graph did not open: %s", session->Path().c_str(), t.graphError.c_str());
    }
    t.session = std::move(session);
    m_tabs.push_back(std::move(t));
    m_active = m_focusTab = static_cast<int>(m_tabs.size()) - 1;
    m_open = true;
    LogOpen(*m_tabs.back().session);
    return true;
}

bool MaterialEditorWindow::OpenEntityNow(World& world, Entity e, std::string& why)
{
    if (e == NullEntity || !world.IsAlive(e)) { why = "the entity is not alive"; return false; }
    const MaterialComponent* mat = world.GetComponent<MaterialComponent>(e);
    if (!mat) { why = "the entity has no MaterialComponent"; return false; }
    const EntityHandle h = world.MakeHandle(e);
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        if (m_tabs[i].session->IsEntity() && m_tabs[i].entity == h)
        {
            m_open = true;
            m_active = m_focusTab = i;
            return true;
        }
    if (static_cast<int>(m_tabs.size()) >= kMaxTabs)
    {
        why = "already " + std::to_string(kMaxTabs) + " tabs open - close one first";
        return false;
    }
    Tab t;
    t.session = std::make_unique<MaterialEditSession>();
    t.session->OpenEntity(world.GetName(e), SourceOf(world, e), *mat, OverrideKeysOf(world, e));
    t.entity     = h;
    t.entityName = world.GetName(e);
    t.epoch      = world.Epoch();
    if (const GuidComponent* g = world.GetComponent<GuidComponent>(e)) t.guid = g->guid;
    t.name = t.entityName + " (entity)";
    m_tabs.push_back(std::move(t));
    m_active = m_focusTab = static_cast<int>(m_tabs.size()) - 1;
    m_open = true;
    const MaterialEditSession& s = *m_tabs.back().session;
    LOG_INFO("[MATEDITOR] open %s: kind=entity source=%s overrides=%zu keys=%s tabs=%zu", s.Path().c_str(),
             s.Info().parentPath.empty() ? "-" : s.Info().parentPath.c_str(), s.Info().overrideKeys.size(),
             JoinSorted(Sorted(s.Info().overrideKeys)).c_str(), m_tabs.size());
    return true;
}

bool MaterialEditorWindow::ActiveEntity(EntityHandle& out) const
{
    if (m_active < 0 || m_active >= static_cast<int>(m_tabs.size()) || !m_tabs[m_active].session->IsEntity())
        return false;
    out = m_tabs[m_active].entity;
    return true;
}

bool MaterialEditorWindow::OwnsUndoShortcut() const
{
    if (!m_focused) return false;
    // An entity tab's edits are steps of the scene's history: Ctrl+Z goes there.
    return !(m_active >= 0 && m_active < static_cast<int>(m_tabs.size()) && m_tabs[m_active].session->IsEntity());
}

bool MaterialEditorWindow::CloseTab(int tab, bool discard, std::string& why)
{
    if (tab < 0) tab = m_active;
    if (tab < 0 || tab >= static_cast<int>(m_tabs.size())) { why = "no such tab"; return false; }
    if (TabDirty(m_tabs[tab]) && !discard)
    {
        why = "'" + m_tabs[tab].session->Path() + "' has unsaved edits - Save, or close with {discard = true}";
        return false;
    }
    LOG_INFO("[MATEDITOR] close %s: dirty=%d", m_tabs[tab].session->Path().c_str(), TabDirty(m_tabs[tab]) ? 1 : 0);
    // A live tab closed unsaved: the scene goes back to the file (in PPA, where the entities
    // may be touched - SyncLive).
    if (m_tabs[tab].live) m_liveEnded.push_back(m_tabs[tab].session->Path());
    m_tabs.erase(m_tabs.begin() + tab);
    if (m_active >= static_cast<int>(m_tabs.size())) m_active = static_cast<int>(m_tabs.size()) - 1;
    else if (m_active > tab) --m_active;
    m_focusTab = m_active;
    return true;
}

bool MaterialEditorWindow::CloseAll(bool discard, std::string& why)
{
    if (!discard && AnyDirty()) { why = "a tab has unsaved edits - Save, or close with {discard = true}"; return false; }
    while (!m_tabs.empty()) CloseTab(static_cast<int>(m_tabs.size()) - 1, true, why);
    m_active = -1;
    return true;
}

bool MaterialEditorWindow::Activate(int tab)
{
    if (tab < 0 || tab >= static_cast<int>(m_tabs.size())) return false;
    m_active = m_focusTab = tab;
    return true;
}

bool MaterialEditorWindow::SetViewMode(const std::string& name, std::string& why)
{
    std::uint32_t mode = 0;
    if (!MeshEditorWindow::ParseViewMode(Lower(name), mode))
    {
        why = "view is lit, unlit, wireframe, basecolor, normal, roughness, metallic, ao or depth";
        return false;
    }
    m_preview->SetViewMode(mode);
    return true;
}

// ==== Material graph ====
MaterialGraphDocument* MaterialEditorWindow::ActiveGraph()
{
    Tab* t = ActiveTabPtr();
    return t && t->graph ? t->graph.get() : nullptr;
}

MaterialGraph::CompileOptions MaterialEditorWindow::GraphOptions(const Tab& t) const
{
    MaterialGraph::CompileOptions o;
    o.shadingModel = t.session->Mat().customShadingModel;
    o.blendMode    = t.session->Mat().userBlendMode;
    if (t.graph) o.sourceLabel = t.graph->Path();
    for (const auto& [name, on] : t.session->Mat().staticSwitches) o.switches[name] = on;
    return o;
}

MaterialGraph::CompileOptions MaterialEditorWindow::LiveOptions(const Tab& t) const
{
    MaterialGraph::CompileOptions o = GraphOptions(t);
    if (t.previewNode)
    {
        // The previewed node's output, unlit and opaque (the preview material matches).
        o.previewNode  = t.previewNode;
        o.previewPin   = t.previewPin;
        o.shadingModel = ShadingModel::Unlit;
        o.blendMode    = BlendMode::Opaque;
    }
    return o;
}

MaterialGraph::CompileOptions MaterialEditorWindow::ActiveGraphOptions() const
{
    if (m_active < 0 || m_active >= static_cast<int>(m_tabs.size())) return {};
    return GraphOptions(m_tabs[m_active]);
}

bool MaterialEditorWindow::SetPreviewOn(Tab& t, std::uint32_t node, const std::string& pin, std::string& why)
{
    if (!t.graph) { why = "not a graph tab"; return false; }
    std::string p = pin;
    if (node)
    {
        const MaterialGraph::Node* n = t.graph->Graph().FindNode(node);
        const MaterialGraph::NodeDesc* d = n ? MaterialGraph::DescribeNode(*n) : nullptr;
        if (!d) { why = "unknown_node: no node " + std::to_string(node); return false; }
        if (d->flags & MaterialGraph::kNodeOutput) node = 0;   // the Output previews the material
        else
        {
            if (p.empty() && !d->outputs.empty()) p = d->outputs.front().name;
            if (d->OutputIndex(p) < 0) { why = "unknown_pin: node " + std::to_string(node) + " has no output '" + p + "'"; return false; }
        }
    }
    if (!node) p.clear();
    if (t.previewNode == node && t.previewPin == p) return true;
    t.previewNode = node;
    t.previewPin  = p;
    // Compile now: the graph did not change, what it is compiled for did.
    t.graphLive.builtRevision = 0;
    t.graphLive.changedAt     = SteadySeconds() - kLiveDebounceSeconds;
    if (node) LOG_INFO("[MATGRAPH] preview %s: node #%u .%s", t.graph->Path().c_str(), node, p.c_str());
    else      LOG_INFO("[MATGRAPH] preview %s: off (the material)", t.graph->Path().c_str());
    return true;
}

bool MaterialEditorWindow::SetGraphPreview(std::uint32_t node, const std::string& pin, std::string& why)
{
    Tab* t = ActiveTabPtr();
    if (!t || !t->graph) { why = "the active tab has no graph"; return false; }
    return SetPreviewOn(*t, node, pin, why);
}

std::pair<std::uint32_t, std::string> MaterialEditorWindow::GraphPreview() const
{
    if (m_active < 0 || m_active >= static_cast<int>(m_tabs.size())) return {};
    const Tab& t = m_tabs[m_active];
    return { t.previewNode, t.previewPin };
}

bool MaterialEditorWindow::SetGraphHideUnrelated(bool on)
{
    Tab* t = ActiveTabPtr();
    if (!t || !t->graph) return false;
    if (!t->canvas) t->canvas = std::make_unique<MaterialGraphCanvas>();
    t->canvas->SetHideUnrelated(on);
    return true;
}

bool MaterialEditorWindow::GraphHideUnrelated() const
{
    if (m_active < 0 || m_active >= static_cast<int>(m_tabs.size())) return false;
    const Tab& t = m_tabs[m_active];
    return t.canvas && t.canvas->HideUnrelated();
}

MaterialEditorWindow::GraphLiveView MaterialEditorWindow::ActiveGraphLive() const
{
    GraphLiveView v;
    if (m_active < 0 || m_active >= static_cast<int>(m_tabs.size()) || !m_tabs[m_active].graph) return v;
    const GraphLive& L = m_tabs[m_active].graphLive;
    v.active    = L.active;
    v.pending   = L.job.valid();
    v.built     = L.builtRevision;
    v.edit      = L.editPath;
    v.compiles  = L.compiles;
    v.failures  = L.failures;
    v.lastMs    = L.lastMs;
    v.dxcErrors = L.dxc.size();
    for (const MaterialGraph::Diagnostic& d : L.dxc) v.dxc.emplace_back(d.node, d.message);
    return v;
}

const MaterialComponent& MaterialEditorWindow::PreviewMaterial(Tab& t)
{
    MaterialEditSession& s = *t.session;
    GraphLive& L = t.graphLive;
    if (!t.graph || !L.active) return s.Mat();
    const std::uint64_t key = (s.Revision() << 24) ^ L.version ^ (static_cast<std::uint64_t>(t.previewNode) << 44);
    if (L.materialFor != key)
    {
        // The staged material pointed at the live shader, with the textures the live graph
        // samples bound (ApplyGraph's rule: a fixed texture's path, an unset parameter's default):
        // what Save will make of it.
        L.material = s.Mat();
        L.material.useCustomShader  = true;
        L.material.customShaderPath = L.editPath;
        L.material.materialGraph    = t.graph->Path();
        if (t.previewNode)
        {
            // A previewed node is shown unlit and opaque (LiveOptions compiles it so).
            L.material.customShadingModel = ShadingModel::Unlit;
            L.material.userBlendMode      = BlendMode::Opaque;
        }
        const MaterialGraph::CompileResult& r = t.graph->Compiled(GraphOptions(t));
        for (const MaterialGraph::TextureInfo& tex : r.textures)
        {
            if (tex.defaultPath.empty()) continue;
            auto it = L.material.customTextures.find(tex.hlslName);
            const bool unset = it == L.material.customTextures.end() || it->second.name.empty();
            if (unset || !tex.parameter)
            {
                MaterialComponent::TextureMap tm;
                tm.name = tex.defaultPath;
                L.material.customTextures[tex.hlslName] = tm;
            }
        }
        L.material.SetDirty();
        L.materialFor = key;
    }
    return L.material;
}

void MaterialEditorWindow::SyncGraphLive(const MaterialEditorContext& ctx)
{
    // -matgraph_live_off, the reverse arm: the unsaved graph reaches nothing before Save.
    static const bool s_off = Sys::CommandLine::Get().Has("-matgraph_live_off");
    const double now = SteadySeconds();
    for (Tab& t : m_tabs)
    {
        if (!t.graph) continue;
        MaterialGraphDocument& g = *t.graph;
        GraphLive& L = t.graphLive;
        if (L.editPath.empty()) L.editPath = LiveShaderPathFor(t.session->Path(), g.Path());
        if (t.previewNode && !g.Graph().FindNode(t.previewNode))   // the previewed node went (deleted, undone)
        {
            std::string why;
            SetPreviewOn(t, 0, "", why);
        }

        // Saved while live: retire the live shader once the file's shader reloaded.
        if (L.retiring)
        {
            const ShaderLibrary* lib = ctx.renderer ? ctx.renderer->GetMaterialShaderLibrary() : nullptr;
            if (!lib || lib->GetReloadEpoch() != L.retireEpoch || now > L.retireBy)
            {
                L.retiring = false;
                L.active   = false;
                ++L.version;
                LOG_INFO("[MATGRAPH] live %s: off (saved; its file's shader is in effect)", g.Path().c_str());
            }
        }

        // A worker's compile finished: the source first, then its cache (never older
        // than the source), so the draws that follow load it instead of compiling.
        if (L.job.valid() && L.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            LiveCompileResult res = L.job.get();
            ++L.compiles;
            L.lastMs = res.ms;
            bool wrote = res.ok;
            // Saved meanwhile with these very bytes: the material's own file is the shader.
            std::string official;
            const MaterialComponent& sm = t.session->Mat();
            if (res.ok && !L.active && sm.useCustomShader && Sys::ReadFileText(sm.customShaderPath, official) &&
                WithoutCR(official) == L.pendingHlsl)
            {
                L.hlsl = std::move(L.pendingHlsl);
                continue;
            }
            if (res.ok)
            {
                Sys::CreateParentDirs(L.editPath);
                wrote = Sys::WriteFileBytesAtomic(L.editPath, L.pendingHlsl.data(), L.pendingHlsl.size());
                if (wrote)
                    for (const auto& [cachePath, blob] : res.containers)
                    {
                        Sys::CreateParentDirs(cachePath);
                        Sys::WriteFileBytesAtomic(cachePath, blob.data(), blob.size());
                    }
            }
            if (wrote)
            {
                const bool first = !L.active;
                L.hlsl     = std::move(L.pendingHlsl);
                L.active   = true;
                L.retiring = false;   // edited again right after a Save: this is the preview's now
                L.dxc.clear();
                ++L.version;
                // Loaded before (an earlier edit, or one before a Save whose bytes the library still
                // holds): reload it, with the same pre-flight and receipt. A file no watcher sees has to
                // ask.
                const ShaderLibrary* lib = ctx.renderer ? ctx.renderer->GetMaterialShaderLibrary() : nullptr;
                if (lib && lib->FindDynamic(L.editPath.c_str(), RHI::ShaderStage::PS) != ShaderLibrary::kInvalidDynShaderID &&
                    !Watched(L.editPath))
                    HotReload::RequestShaderReload(L.editPath);
                LOG_INFO("[MATGRAPH] live %s: ok dxcMs=%.0f perms=%d bytes=%zu edit=%s%s", g.Path().c_str(), res.ms,
                         res.perms, L.hlsl.size(), L.editPath.c_str(), first ? " (on)" : "");
            }
            else
            {
                ++L.failures;
                L.dxc = res.ok ? std::vector<MaterialGraph::Diagnostic>{}
                               : MaterialGraph::MapDxcDiagnostics(res.error, L.pendingHlsl, L.editPath);
                if (res.ok)
                {
                    MaterialGraph::Diagnostic d;
                    d.code    = "dxc";
                    d.message = "cannot write '" + L.editPath + "'";
                    L.dxc.push_back(std::move(d));
                }
                L.pendingHlsl.clear();
                LOG_WARNING("[MATGRAPH] live %s: failed dxcMs=%.0f errors=%zu first=%s", g.Path().c_str(), res.ms,
                            L.dxc.size(), L.dxc.empty() ? "-" : L.dxc.front().message.c_str());
            }
        }
        if (s_off) continue;
        // A function it calls, saved anywhere, is an edit of it too (checked every 250 ms).
        MaterialGraph::TouchCalledFunctions(g.Graph());
        const std::uint64_t epoch = MaterialGraph::FunctionEpoch();
        // ...and so is what the material compiles it with: its switches, shading model, blend mode
        // (the staged material's revision; a value edit recompiles nothing: the bytes compare
        // below).
        const std::uint64_t matRev = t.session->Revision();
        if (g.Revision() != L.seenRevision || epoch != L.seenEpoch || matRev != L.seenMaterial)
        {
            L.seenRevision = g.Revision();
            L.seenEpoch    = epoch;
            L.seenMaterial = matRev;
            L.changedAt    = now;
        }
        // One compile at a time, after the edits settle: a dragged value moves the revision every
        // frame, so this waits for the drag to rest.
        if (L.job.valid() || (L.builtRevision == g.Revision() && L.builtEpoch == epoch && L.builtMaterial == matRev) ||
            now - L.changedAt < kLiveDebounceSeconds)
            continue;
        L.builtRevision = g.Revision();
        L.builtEpoch    = epoch;
        L.builtMaterial = matRev;
        const MaterialGraph::CompileResult& r = g.Compiled(LiveOptions(t));
        if (!r.ok) continue;   // its errors are on the nodes
        if (r.hlsl == L.hlsl)
        {
            // Back to the bytes the live shader already has (an edit undone, a DXC error fixed back):
            // nothing to recompile, and a DXC error of other bytes is stale.
            L.dxc.clear();
            continue;
        }
        if (!t.previewNode && !L.retiring)
        {
            // The material's own shader may be exactly these bytes (the tab just opened or saved, an
            // edit undone, a node preview stopped): nothing to preview, the saved shader is the look.
            std::string current;
            const MaterialComponent& m = t.session->Mat();
            if (m.useCustomShader && m.customShaderPath == ShaderPathForCombination(g.Path(), r.switches, MaterialKey(g.Graph(), m)) &&
                Sys::ReadFileText(m.customShaderPath, current) && WithoutCR(current) == r.hlsl)
            {
                L.hlsl = r.hlsl;
                if (L.active)
                {
                    L.active = false;
                    ++L.version;
                    LOG_INFO("[MATGRAPH] live %s: off (the saved shader)", g.Path().c_str());
                }
                continue;
            }
        }
        const ShaderLibrary* lib = ctx.renderer ? ctx.renderer->GetMaterialShaderLibrary() : nullptr;
        if (!lib) continue;
        L.pendingHlsl = r.hlsl;
        MaterialComponent drawnAs = t.session->Mat();   // what the preview draws it as
        if (t.previewNode)
        {
            drawnAs.customShadingModel = ShadingModel::Unlit;
            drawnAs.userBlendMode      = BlendMode::Opaque;
        }
        const auto perms = LivePermutations(drawnAs, r.stats.usesVertex);
        LOG_INFO("[MATGRAPH] live %s: compiling revision=%llu perms=%zu", g.Path().c_str(),
                 static_cast<unsigned long long>(g.Revision()), perms.size());
        L.job = std::async(std::launch::async, [lib, path = L.editPath, hlsl = r.hlsl, perms]() {
            LiveCompileResult out;
            const auto t0 = std::chrono::steady_clock::now();
            out.ok = true;
            for (const auto& [stage, bits] : perms)
            {
                PermutationKey pk;
                pk.bits = bits;
                ShaderLibrary::CacheCompile c = lib->CompileForCache(path, hlsl, stage, pk);
                if (!c.ok)
                {
                    out.ok = false;
                    out.error = c.error;
                    break;
                }
                out.containers.emplace_back(std::move(c.cachePath), std::move(c.container));
                ++out.perms;
            }
            out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return out;
        });
    }
}

std::string MaterialEditorWindow::ActiveGraphError() const
{
    return (m_active >= 0 && m_active < static_cast<int>(m_tabs.size())) ? m_tabs[m_active].graphError : std::string();
}

bool MaterialEditorWindow::CreateGraph(const std::string& from, std::string& why)
{
    Tab* t = ActiveTabPtr();
    if (!t) { why = "no material is open"; return false; }
    MaterialEditSession& s = *t->session;
    if (s.IsEntity()) { why = "an entity tab has no file to give a graph: open its material"; return false; }
    if (s.IsInstance()) { why = "an instance edits its parent's parameters: give the PARENT a graph"; return false; }
    if (t->graph) { why = "it has a graph already: " + t->graph->Path(); return false; }
    if (!s.Mat().materialGraph.empty())
    {
        why = "it names a graph that did not open (" + t->graphError + ")";
        return false;
    }
    // A hand-written custom shader stays code: a graph would be the engine's standard surface
    // (or nothing), not that shader. Its parameters come from reflection; "Open .hlsl" edits
    // it.
    if (s.Mat().useCustomShader && !s.Mat().customShaderPath.empty())
    {
        why = "custom_hlsl: '" + s.Mat().customShaderPath + "' is hand-written HLSL - it stays code (Open .hlsl); "
              "Create Graph converts a standard material";
        return false;
    }
    if (from != "standard" && from != "empty") { why = "from is \"standard\" or \"empty\""; return false; }
    const ECS::Guid guid = Resource::AssetGuidIndex::Get().EnsureGuid(s.Path());
    if (!guid.IsValid()) { why = "the material has no guid (.meta) to name its graph by"; return false; }
    const std::string hex  = guid.ToHex();
    const std::string path = GeneratedDirFor(s.Path()) + "/" + StemOf(s.Path()) + "_" + hex.substr(0, 8) + ".imatgraph";
    if (Sys::FileExists(path))
    {
        why = "'" + path + "' exists already (a graph this material once had?): name it in the material's "
              "materialGraph key, or delete it";
        return false;
    }
    auto g = std::make_unique<MaterialGraphDocument>();
    g->New(path, hex);
    if (from == "standard")
    {
        // The material as it renders now: the engine's standard surface into every pin its
        // shading model and blend mode use (world-space normal).
        const std::uint32_t out = g->OutputId();
        const ShadingModel sm = s.Mat().customShadingModel;
        const BlendMode    bm = s.Mat().userBlendMode;
        std::vector<GraphOp> ops;
        GraphOp add;
        add.kind   = GraphOp::Kind::Add;
        add.type   = "StandardSurface";
        add.as     = "std";
        add.pos[0] = 0.0f;
        ops.push_back(add);
        GraphOp set;
        set.kind = GraphOp::Kind::Set;
        set.node = out;
        set.props["tangentSpaceNormal"] = MaterialGraph::Value::Bool(false);
        ops.push_back(set);
        auto connect = [&](const char* fromPin, const char* toPin)
        {
            GraphOp c;
            c.kind    = GraphOp::Kind::Connect;
            c.fromRef = "std";
            c.fromPin = fromPin;
            c.to      = out;
            c.toPin   = toPin;
            ops.push_back(c);
        };
        // An Opaque material's Opacity Mask is its cutout (alphaRef decides); a deferred Unlit's
        // Base Color shows under its Emissive. OutputPinActive has both.
        for (const char* pin : { "BaseColor", "Metallic", "Specular", "Roughness", "EmissiveColor", "Normal",
                                 "AmbientOcclusion" })
            if (MaterialGraph::OutputPinActive(pin, sm, bm)) connect(pin, pin);
        if (MaterialGraph::OutputPinActive("Opacity", sm, bm)) connect("Opacity", "Opacity");
        else if (MaterialGraph::OutputPinActive("OpacityMask", sm, bm)) connect("Opacity", "OpacityMask");
        if (!g->Apply(ops, nullptr, why)) return false;
        g->ClearHistory();
    }
    LOG_INFO("[MATGRAPH] create %s: from=%s graph=%s nodes=%zu links=%zu", s.Path().c_str(), from.c_str(),
             path.c_str(), g->Graph().nodes.size(), g->Graph().links.size());
    t->graph = std::move(g);
    t->graphError.clear();
    return true;
}

bool MaterialEditorWindow::CreateFunction(const std::string& pathIn, std::string& why)
{
    const std::string path = Canon(pathIn);
    if (Lower(std::filesystem::path(Sys::U8Path(path)).extension().string()) != ".imatfunc")
    {
        why = "a Material Function's file ends in .imatfunc";
        return false;
    }
    if (Sys::FileExists(path)) { why = "'" + path + "' exists already: open it"; return false; }
    if (static_cast<int>(m_tabs.size()) >= kMaxTabs)
    {
        why = "already " + std::to_string(kMaxTabs) + " tabs open - close one first";
        return false;
    }
    auto g = std::make_unique<MaterialGraphDocument>();
    g->NewFunction(path);
    auto session = std::make_unique<MaterialEditSession>();
    session->OpenFunction(path);
    Tab t;
    t.name    = BaseName(path);
    t.graph   = std::move(g);
    t.session = std::move(session);
    m_tabs.push_back(std::move(t));
    m_active = m_focusTab = static_cast<int>(m_tabs.size()) - 1;
    m_open = true;
    LOG_INFO("[MATGRAPH] create function %s: nodes=2 links=1", path.c_str());
    return true;
}

bool MaterialEditorWindow::RevertActive(std::string& why)
{
    Tab* t = ActiveTabPtr();
    if (!t) { why = "no material is open"; return false; }
    if (!t->session->Revert(why)) return false;
    RevertGraph(*t);
    t->diskChanged = false;
    return true;
}

void MaterialEditorWindow::RevertGraph(Tab& t)
{
    // The material names its graph again as the file says: re-read it, open the one it now
    // names, or drop a created-but-never-saved one. Its live shader goes: the preview reads
    // the file again.
    if (t.graphLive.job.valid()) t.graphLive.job.wait();
    {
        const std::string edit = t.graphLive.editPath;
        const std::uint64_t version = t.graphLive.version;
        t.graphLive = GraphLive{};
        t.graphLive.editPath = edit;
        t.graphLive.version  = version + 1;
    }
    t.previewNode = 0;   // the material again
    t.previewPin.clear();
    const std::string named = t.session->IsInstance() ? std::string() : t.session->Mat().materialGraph;
    std::string why;
    if (named.empty()) { t.graph.reset(); t.graphError.clear(); return; }
    if (t.graph && t.graph->Path() == named)
    {
        if (!t.graph->Revert(why)) LOG_WARNING("[MATGRAPH] revert %s: %s", named.c_str(), why.c_str());
        return;
    }
    auto g = std::make_unique<MaterialGraphDocument>();
    if (g->Open(named, why)) { t.graph = std::move(g); t.graphError.clear(); }
    else                     { t.graph.reset(); t.graphError = why; }
}

bool MaterialEditorWindow::ApplyGraph(const MaterialEditorContext& ctx, Tab& t, std::string& json, std::string& why)
{
    MaterialEditSession& s = *t.session;
    MaterialGraphDocument& g = *t.graph;
    const MaterialGraph::CompileResult& r = g.Compiled(GraphOptions(t));   // the material, its switches
    int warnings = 0;
    const MaterialGraph::Diagnostic* firstError = nullptr;
    for (const MaterialGraph::Diagnostic& d : r.diagnostics)
    {
        if (d.severity == MaterialGraph::Severity::Warning) ++warnings;
        if (d.severity == MaterialGraph::Severity::Error && !firstError) firstError = &d;
    }
    if (!r.ok)
    {
        why = "graph_errors: the graph does not compile: " + (firstError ? firstError->message : std::string("?")) +
              (firstError && firstError->node ? " (node #" + std::to_string(firstError->node) + ")" : std::string());
        LOG_WARNING("[MATGRAPH] apply %s: ok=0 errors=%zu first=%s", s.Path().c_str(),
                    r.diagnostics.size() - static_cast<std::size_t>(warnings), firstError ? firstError->code.c_str() : "-");
        return false;
    }
    if (g.IsFunction())
    {
        // A Material Function is its graph: saved, with no shader and no material. The graphs
        // calling it compile it in (their compile sees the function epoch move).
        if ((g.Dirty() || !Sys::FileExists(g.Path())) && !g.Save(why)) return false;
        json = "{\"file\":\"" + JsonEscape(g.Path()) + "\",\"function\":true,\"graphHash\":\"" + r.graphHash + "\"}";
        LOG_INFO("[MATGRAPH] apply %s: ok=1 function=1 graphHash=%s", g.Path().c_str(), r.graphHash.c_str());
        return true;
    }
    // The HLSL only when its bytes change (moving a node or a comment recompiles nothing).
    // The file of this switch combination - at most kMaxSwitchCombinations per graph - and,
    // for a graph with no owner, of this material's shading model / blend mode.
    const std::string hlsl = ShaderPathForCombination(g.Path(), r.switches, MaterialKey(g.Graph(), s.Mat()));
    if (hlsl != ShaderPathForGraph(g.Path()) && !Sys::FileExists(hlsl) && CombinationFiles(g.Path()) >= kMaxSwitchCombinations)
    {
        why = "switch_combinations: '" + g.Path() + "' has " + std::to_string(kMaxSwitchCombinations) +
              " static switch combinations already (each is a shader for good): reuse one, or delete the unused files";
        return false;
    }
    std::string current;
    const bool same = Sys::ReadFileText(hlsl, current) && WithoutCR(current) == r.hlsl;
    if (!same)
    {
        Sys::CreateParentDirs(hlsl);
        bool locked = false;
        if (!Sys::WriteFileBytesAtomic(hlsl, r.hlsl.data(), r.hlsl.size(), &locked))
        {
            why = locked ? "'" + hlsl + "' stayed locked by a reader" : "cannot write '" + hlsl + "'";
            return false;
        }
        // Where no watcher looks (a harness copy), say it changed: what loaded it reloads.
        if (!Watched(hlsl)) HotReload::RequestShaderReload(hlsl);
    }
    if ((g.Dirty() || !Sys::FileExists(g.Path())) && !g.Save(why)) return false;
    // The graph's other combinations (other materials, instances) follow it.
    const CombinationRefresh others = RefreshCombinations(g.Graph(), g.Path(), hlsl);

    // Point the material at them through the loader's parser, one undo step. A fixed
    // texture's path is the graph's; a parameter's default seeds an unset slot.
    std::vector<std::pair<std::string, std::string>> kv;
    const MaterialComponent& m = s.Mat();
    if (!m.useCustomShader) kv.emplace_back("useCustomShader", "1");
    if (m.customShaderPath != hlsl) kv.emplace_back("customShaderPath", hlsl);
    if (m.materialGraph != g.Path()) kv.emplace_back("materialGraph", g.Path());
    for (const MaterialGraph::TextureInfo& tex : r.textures)
    {
        if (tex.defaultPath.empty()) continue;
        const auto it = m.customTextures.find(tex.hlslName);
        const bool unset = it == m.customTextures.end() || it->second.name.empty();
        if (unset || (!tex.parameter && it->second.name != tex.defaultPath))
            kv.emplace_back("ctex_" + tex.hlslName, "0 " + tex.defaultPath);
    }
    if (!kv.empty() && !s.Set(kv, why)) return false;

    // The file the material names now holds the shader the preview showed, so the live one
    // goes (the next edit makes a new one). If that file's OLD shader is loaded, it reloads
    // first (the scene switches then); until then the preview keeps the live one (the same
    // bytes), or it would show the old look for a moment. A previewed node stays previewed:
    // the live shader is that node's, not these bytes.
    if (t.graphLive.active && !t.previewNode)
    {
        const ShaderLibrary* lib = ctx.renderer ? ctx.renderer->GetMaterialShaderLibrary() : nullptr;
        if (!same && lib && lib->FindDynamic(hlsl.c_str(), RHI::ShaderStage::PS) != ShaderLibrary::kInvalidDynShaderID)
        {
            t.graphLive.retiring    = true;
            t.graphLive.retireEpoch = lib->GetReloadEpoch();
            t.graphLive.retireBy    = SteadySeconds() + 5.0;
        }
        else
        {
            t.graphLive.active = false;
            ++t.graphLive.version;
        }
    }
    if (!t.previewNode) t.graphLive.hlsl = r.hlsl;
    json = "{\"file\":\"" + JsonEscape(g.Path()) + "\",\"hlsl\":\"" + JsonEscape(hlsl) + "\",\"hlslChanged\":" +
           (same ? "false" : "true") + ",\"graphHash\":\"" + r.graphHash + "\",\"bytes\":" +
           std::to_string(r.stats.hlslBytes) + ",\"params\":" + std::to_string(r.stats.params) + ",\"textures\":" +
           std::to_string(r.stats.textures) + ",\"warnings\":" + std::to_string(warnings) +
           ",\"combinations\":{\"files\":" + std::to_string(others.files) + ",\"rewritten\":" +
           std::to_string(others.rewritten) + ",\"failed\":" + std::to_string(others.failed) + "}}";
    LOG_INFO("[MATGRAPH] apply %s: ok=1 graph=%s hlsl=%s changed=%d bytes=%d params=%d textures=%d warnings=%d "
             "graphHash=%s", s.Path().c_str(), g.Path().c_str(), hlsl.c_str(), same ? 0 : 1, r.stats.hlslBytes,
             r.stats.params, r.stats.textures, warnings, r.graphHash.c_str());
    if (others.files)
        LOG_INFO("[MATGRAPH] combinations %s: files=%d rewritten=%d failed=%d", g.Path().c_str(), others.files,
                 others.rewritten, others.failed);
    return true;
}

MaterialEditorWindow::StateView MaterialEditorWindow::State() const
{
    StateView v;
    v.open   = m_open;
    v.active = m_active;
    v.viewId = m_preview->ViewId();
    for (const Tab& t : m_tabs)
    {
        const MaterialEditSession& s = *t.session;
        TabState ts;
        ts.path           = s.Path();
        ts.parent         = s.Info().parentPath;
        ts.kind           = t.graph && !s.IsFunction() ? "graph" : MaterialKindName(s.Kind());   // a function tab: "function"
        ts.dirty          = TabDirty(t);
        ts.graph          = t.graph ? t.graph->Path() : std::string();
        ts.graphDirty     = t.graph && t.graph->Dirty();
        ts.instance       = s.IsInstance();
        ts.explicitFormat = s.Info().explicitFormat;
        ts.overrides      = Sorted(s.Info().overrideKeys);
        ts.unset          = s.Info().unsetKeys;
        std::sort(ts.unset.begin(), ts.unset.end());
        ts.undo           = s.UndoDepth();
        ts.redo           = s.RedoDepth();
        ts.diskChanged    = t.diskChanged;
        ts.live           = t.live;
        ts.entity         = s.IsEntity() ? t.entity : NullEntityHandle;
        if (!s.IsFunction())
        {
            // What rays and shadows see of it.
            MaterialGraphDocument* g = t.graph ? t.graph.get() : nullptr;
            ts.rtProxy       = RtProxySource(s.Mat(), m_renderer);
            ts.shadowMaskGap = ShadowMaskGap(s.Mat(), g);
            if (ts.shadowMaskGap && g) ts.mirrorMask = MirrorMaskPath(g->Compiled(GraphOptions(t)), s.Mat());
        }
        v.tabs.push_back(std::move(ts));
    }
    v.livePushes = m_livePushes;
    return v;
}

// ==== Saves ====
std::uint64_t MaterialEditorWindow::QueueSave(const std::string& as, bool instance, std::uint32_t requestId)
{
    PendingSave s;
    s.tab      = m_active;
    s.as       = as;
    s.instance = instance;
    s.job      = Harness::Jobs::TryCreate("mateditor_save", as.empty() ? "save" : as, requestId);
    if (!s.job) return 0;
    Harness::Jobs::Start(s.job);
    m_pendingSaves.push_back(std::move(s));
    return m_pendingSaves.back().job;
}

void MaterialEditorWindow::ReloadDependents(const std::string& saved)
{
    // Every open tab that reads @p saved: never the file itself in another tab, an instance of
    // it (at any depth) yes. A clean tab follows silently; a dirty one is told (the banner) and
    // keeps the user's edits.
    const std::vector<std::string> deps = Resource::MaterialsDependingOn(saved);
    for (Tab& t : m_tabs)
    {
        MaterialEditSession& s = *t.session;
        if (Lower(s.Path()) == Lower(saved)) continue;
        const bool reads = std::any_of(deps.begin(), deps.end(),
                                       [&](const std::string& d) { return Lower(d) == Lower(s.Path()); });
        if (!reads) continue;
        std::string why;
        if (!s.Dirty() && s.Revert(why))
            LOG_INFO("[MATEDITOR] follow %s: its parent '%s' was saved", s.Path().c_str(), saved.c_str());
        else if (s.Dirty())
            t.diskChanged = true;
    }
}

void MaterialEditorWindow::RunSave(const MaterialEditorContext& ctx, const PendingSave& p)
{
    std::string why;
    if (p.tab < 0 || p.tab >= static_cast<int>(m_tabs.size()))
    {
        Harness::Jobs::Fail(p.job, "no_tab", "the tab closed before the save ran", {});
        return;
    }
    MaterialEditSession& s = *m_tabs[p.tab].session;
    Tab& tab = m_tabs[p.tab];
    const std::string before = s.Path();   // Save As moves the session
    bool ok = false;
    std::string written;
    std::string graphJson;                 // what Apply wrote
    std::vector<std::string> instanceKeys;
    if (p.instance)
    {
        ok = s.SaveAsInstance(p.as, why, &instanceKeys);
        written = Canon(p.as);
    }
    else if (!p.as.empty())
    {
        ok = s.SaveAs(p.as, why);
        written = s.Path();
        if (ok) m_tabs[p.tab].name = BaseName(written);
        // The copy owns a graph of its own, named by ITS guid, and the HLSL generated from it;
        // the original keeps its graph.
        if (ok && tab.graph)
        {
            const ECS::Guid guid = Resource::AssetGuidIndex::Get().EnsureGuid(written);
            const std::string hex = guid.IsValid() ? guid.ToHex() : std::string();
            if (hex.empty()) { ok = false; why = "the copy has no guid to name its graph by"; }
            else
            {
                tab.graph->SetOwner(hex);
                ok = tab.graph->SaveAs(GeneratedDirFor(written) + "/" + StemOf(written) + "_" + hex.substr(0, 8) +
                                       ".imatgraph", why) &&
                     ApplyGraph(ctx, tab, graphJson, why) && s.Save(why);
            }
        }
    }
    else
    {
        ok = (!tab.graph || ApplyGraph(ctx, tab, graphJson, why)) &&
             (!s.IsInstance() || ApplyInstanceSwitches(tab, why)) && s.Save(why);   // an instance's combination
        written = s.Path();
    }
    // What the written file overrides: the new instance's keys, this tab's set, or "-" (standalone).
    const bool asInstanceFile = p.instance || s.IsInstance();
    const std::vector<std::string> keys = p.instance ? instanceKeys : Sorted(s.Info().overrideKeys);
    LOG_INFO("[MATEDITOR] save %s: ok=%d from=%s instance=%d kind=%s overrides=%zu keys=%s%s%s", written.c_str(),
             ok ? 1 : 0, s.Path().c_str(), p.instance ? 1 : 0, p.instance ? "instance" : MaterialKindName(s.Kind()),
             asInstanceFile ? keys.size() : 0, asInstanceFile ? JoinSorted(keys).c_str() : "-", ok ? "" : " why=",
             ok ? "" : why.c_str());
    if (!ok)
    {
        Harness::Jobs::Fail(p.job, "save_failed", why, {});
        return;
    }
    // The file now holds what the overlay served: drop it BEFORE the reload below, so the
    // reload reads the file. Save As leaves the old file as it was and its readers go back to
    // it. (Save as Instance keeps this tab dirty and live.)
    if (!p.instance && m_tabs[p.tab].live)
    {
        Tab& t = m_tabs[p.tab];
        t.live = false;
        t.liveRevision = 0;
        Resource::ClearLiveMaterial(before);
        LOG_INFO("[MATEDITOR] live %s: off (saved%s) pushes=%llu", before.c_str(), before == written ? "" : " as",
                 static_cast<unsigned long long>(t.livePushes));
        t.livePushes = 0;
        if (before != written && ctx.liveChanged) ctx.liveChanged(before, true);
    }
    if (ctx.materialSaved) ctx.materialSaved(written);
    m_tabs[p.tab].diskChanged = false;
    ReloadDependents(written);
    std::string json = "{\"path\":\"" + written + "\",\"instance\":" + (p.instance ? "true" : "false") +
                       ",\"overrides\":" + std::to_string(asInstanceFile ? keys.size() : 0) +
                       (graphJson.empty() ? std::string() : ",\"graph\":" + graphJson) + "}";
    if (p.instance)
    {
        // The new instance, in a tab of its own.
        std::string whyOpen;
        OpenNow(written, whyOpen);
    }
    Harness::Jobs::Succeed(p.job, json);
    if (p.closeAfter)
    {
        std::string whyClose;
        CloseTab(p.tab, false, whyClose);
        if (m_tabs.empty()) m_open = false;
    }
}

// ==== Game half ====
void MaterialEditorWindow::ProcessPendingActions(const MaterialEditorContext& ctx)
{
    m_frame = ctx.editorFrame;
    m_renderer = ctx.renderer;
    // Node thumbnails stream through these; a closed window gives its textures back here, at
    // the serial window (no ImGui frame is drawing them).
    m_gfx    = ctx.gfx;
    m_texSys = ctx.textures;
    m_resMgr = ctx.resources;
    if (!m_open && !m_nodeThumbs.empty()) ReleaseNodeThumbnails();
    m_preview->ProcessPendingActions(ctx.renderer);
    TickHarness(ctx);

    for (const std::string& p : m_pendingOpen)
    {
        std::string why;
        if (!OpenNow(p, why)) LOG_WARNING("[MATEDITOR] cannot open '%s': %s", p.c_str(), why.c_str());
    }
    m_pendingOpen.clear();

    // Undo / redo requests (the Ctrl+Z route, the toolbar): over the canvas the graph's ring,
    // elsewhere the material's.
    if (MaterialEditSession* s = ActiveSession())
    {
        Tab* t = ActiveTabPtr();
        MaterialGraphDocument* g = (m_undoToGraph && t && t->graph) ? t->graph.get() : nullptr;
        for (; m_pendingUndo > 0; --m_pendingUndo) g ? g->Undo() : s->Undo();
        for (; m_pendingRedo > 0; --m_pendingRedo) g ? g->Redo() : s->Redo();
    }
    m_pendingUndo = m_pendingRedo = 0;

    for (int tab : m_pendingRevert)
        if (tab >= 0 && tab < static_cast<int>(m_tabs.size()))
        {
            std::string why;
            if (!m_tabs[tab].session->Revert(why)) LOG_WARNING("[MATEDITOR] revert: %s", why.c_str());
            RevertGraph(m_tabs[tab]);
            m_tabs[tab].diskChanged = false;
        }
    m_pendingRevert.clear();

    const std::vector<PendingSave> saves = std::move(m_pendingSaves);
    m_pendingSaves.clear();
    for (const PendingSave& s : saves) RunSave(ctx, s);

    // Files changed under the tabs (another editor, git, a script): a clean tab
    // reloads, a dirty one shows the banner.
    if (m_frame >= m_lastDiskCheck + kDiskCheckFrames)
    {
        m_lastDiskCheck = m_frame;
        for (Tab& t : m_tabs)
        {
            MaterialEditSession& s = *t.session;
            if (t.diskChanged || !(s.ChangedOnDisk() || (t.graph && t.graph->ChangedOnDisk()))) continue;
            std::string why;
            if (!TabDirty(t))
            {
                if (s.Revert(why))
                    LOG_INFO("[MATEDITOR] reload %s: changed on disk", s.Path().c_str());
                RevertGraph(t);
            }
            else
                t.diskChanged = true;
        }
    }
    if (!m_open && !m_tabs.empty() && !AnyDirty())
    {
        std::string why;
        CloseAll(true, why);
    }
    SyncEntityTabs(ctx);
    SyncGraphLive(ctx);   // the preview's live shader
    SyncLive(ctx);
}

// ==== Entity tabs ====
void MaterialEditorWindow::FlushEntity(const MaterialEditorContext& ctx, Tab& t)
{
    const MaterialEditSession::EntityEdges e = t.session->TakeEntityEdges();
    if (!e.Any()) return;
    if (!ctx.entityWrite || !ctx.entityWrite(t.entity, t.session->Mat(), e.began, e.ended))
        LOG_WARNING("[MATEDITOR] %s: the edit did not reach the entity (it is gone)", t.session->Path().c_str());
}

void MaterialEditorWindow::SyncEntityTabs(const MaterialEditorContext& ctx)
{
    if (!ctx.world) return;
    World& w = *ctx.world;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
    {
        Tab& t = m_tabs[i];
        MaterialEditSession& s = *t.session;
        if (!s.IsEntity()) continue;
        // A new world (a scene switch, Play / Stop) re-creates entities: the handle goes stale,
        // and the entity is found again by its Guid, else by its name when exactly one entity has
        // it.
        if (!w.IsHandleValid(t.entity) && w.Epoch() != t.epoch)
        {
            Entity found = t.guid.IsValid() ? ECS::GuidRegistry::Get().Find(t.guid) : NullEntity;
            if (found == NullEntity)
            {
                int named = 0;
                for (Entity c : w.GetEntities())
                    if (w.IsAlive(c) && w.GetName(c) == t.entityName) { found = c; ++named; }
                if (named != 1) found = NullEntity;
            }
            if (found != NullEntity && w.IsAlive(found))
            {
                t.entity = w.MakeHandle(found);
                t.epoch  = w.Epoch();
            }
        }
        const MaterialComponent* mat = w.IsHandleValid(t.entity) ? w.GetComponent<MaterialComponent>(t.entity.entity)
                                                                  : nullptr;
        if (!mat)
        {
            LOG_INFO("[MATEDITOR] close %s: the entity (or its material) is gone", s.Path().c_str());
            std::string why;
            CloseTab(i, true, why);
            --i;
            continue;
        }
        FlushEntity(ctx, t);   // a scripted edit owed to it
        s.SyncFromEntity(SourceOf(w, t.entity.entity), *mat, OverrideKeysOf(w, t.entity.entity));
    }
}

void MaterialEditorWindow::DrawEntityToolbar(const MaterialEditorContext& ctx, Tab& t)
{
    MaterialEditSession& s = *t.session;
    const bool hasAsset = !s.Info().parentPath.empty();
    std::string why;
    ImGui::BeginDisabled(!hasAsset);
    if (ImGui::Button("Save as Instance..."))
    {
        static const char kImatInst[] = "Material Instance (*.imatinst)\0*.imatinst\0All Files\0*.*\0\0";
        const std::string path = SaveFileDialog(kImatInst, "imatinst", "asset/");
        if (!path.empty() && ctx.entityAction && !ctx.entityAction(t.entity, "promote", path, why))
            LOG_WARNING("[MATEDITOR] save as instance: %s", why.c_str());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("This entity's material as a new .imatinst of %s:\nits overrides move into the file "
                          "and the entity points at it.", hasAsset ? s.Info().parentPath.c_str() : "its asset");
    ImGui::SameLine();
    if (ImGui::Button("Apply to Asset")) ImGui::OpenPopup("##applyasset");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Write this entity's overrides into %s:\nevery entity using it changes.",
                          hasAsset ? s.Info().parentPath.c_str() : "its asset");
    ImGui::SameLine();
    if (ImGui::Button("Clear Overrides") && ctx.entityAction && !ctx.entityAction(t.entity, "clear", {}, why))
        LOG_WARNING("[MATEDITOR] clear overrides: %s", why.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Drop this entity's overrides: the asset's values again (one undo step)");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##applyasset"))
    {
        ImGui::Text("Write %zu override(s) into %s?", s.Info().overrideKeys.size(), s.Info().parentPath.c_str());
        ImGui::TextDisabled("Every entity using the file changes; the file cannot be undone.");
        if (ImGui::Button("Apply"))
        {
            if (ctx.entityAction && !ctx.entityAction(t.entity, "apply", {}, why))
                LOG_WARNING("[MATEDITOR] apply to asset: %s", why.c_str());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.f, 16.f);
    ImGui::TextDisabled("Undo: Ctrl+Z (the scene's history)");
}

// ==== Live overlay (Resource/MaterialLiveOverlay.h) ====
void MaterialEditorWindow::EndLive(const MaterialEditorContext& ctx, Tab& t, const char* why)
{
    const std::string& path = t.session->Path();
    t.live = false;
    t.liveRevision = 0;
    const bool had = Resource::ClearLiveMaterial(path);
    LOG_INFO("[MATEDITOR] live %s: off (%s) pushes=%llu", path.c_str(), why,
             static_cast<unsigned long long>(t.livePushes));
    t.livePushes = 0;
    if (had && ctx.liveChanged) ctx.liveChanged(path, true);
}

void MaterialEditorWindow::SyncLive(const MaterialEditorContext& ctx)
{
    // Closed while live: the file's look again.
    for (const std::string& path : m_liveEnded)
    {
        const bool had = Resource::ClearLiveMaterial(path);
        LOG_INFO("[MATEDITOR] live %s: off (closed)", path.c_str());
        if (had && ctx.liveChanged) ctx.liveChanged(path, true);
    }
    m_liveEnded.clear();

    // -mateditor_live_off, the reverse arm: nothing reaches the scene before Save.
    static const bool s_liveOff = Sys::CommandLine::Get().Has("-mateditor_live_off");
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (Tab& t : m_tabs)
    {
        MaterialEditSession& s = *t.session;
        // The staged material only: a graph's live shader is the preview's; a structural change
        // reaches the scene when it is saved.
        if (!s.Dirty() || s_liveOff)
        {
            if (t.live) EndLive(ctx, t, s_liveOff ? "live off" : "clean");
            continue;
        }
        if (t.live && s.Revision() == t.liveRevision) continue;
        // At most 10 pushes a second while a control is held; at once when it is let go.
        if (t.live && s.InGesture() && now - t.livePushedAt < 0.1) continue;
        Resource::SetLiveMaterial(s.Path(), s.Mat(), s.Info());
        if (!t.live) LOG_INFO("[MATEDITOR] live %s: on", s.Path().c_str());
        t.live         = true;
        t.liveRevision = s.Revision();
        t.livePushedAt = now;
        ++t.livePushes;
        ++m_livePushes;
        if (ctx.liveChanged) ctx.liveChanged(s.Path(), false);
    }
}

void MaterialEditorWindow::ReleaseViewsAtShutdown(Renderer* renderer)
{
    m_preview->ReleaseViewsAtShutdown(renderer);
    ReleaseNodeThumbnails();
}

// ==== Texture nodes' thumbnails ====
std::uint64_t MaterialEditorWindow::NodeThumbnail(const std::string& path)
{
    if (!m_texSys || !m_resMgr || !m_gfx || path.empty()) return 0;
    auto it = m_nodeThumbs.find(path);
    if (it == m_nodeThumbs.end())
    {
        // Acquire inside the ImGui pass, as the Content Browser does for a texture tile: a load is
        // queued, nothing is created here. Bounded: a graph has at most 8 slots, a session of tabs
        // a few dozen textures.
        if (m_nodeThumbs.size() >= kMaxNodeThumbnails) return 0;
        it = m_nodeThumbs.emplace(path, m_texSys->Acquire(path, *m_resMgr, *m_gfx)).first;
    }
    const RHI::Texture* tex = m_texSys->GetTexture(it->second);
    if (!tex) return 0;
    // ImGui's backend samples a Texture2D: a cube / array view there is undefined (as for the
    // browser's tiles).
    const RHI::TextureDesc& d = tex->desc;
    if (d.type != RHI::TextureDesc::Type::TEXTURE_2D || d.arraySize != 1 || d.sampleCount > 1 ||
        RHI::HasFlag(d.miscFlags, RHI::ResourceMiscFlag::TEXTURECUBE))
        return 0;
    return static_cast<GraphicsDX12&>(*m_gfx).GetTexturePreviewSrvGpuHandle(*tex);
}

bool MaterialEditorWindow::NodeThumbnailResident(const std::string& path) const
{
    const auto it = m_nodeThumbs.find(path);
    return m_texSys && it != m_nodeThumbs.end() && m_texSys->GetTexture(it->second) != nullptr;
}

void MaterialEditorWindow::ReleaseNodeThumbnails()
{
    if (m_texSys && m_gfx)
        for (auto& [_, h] : m_nodeThumbs)
            if (h != Resource::kInvalidTextureHandle) m_texSys->Release(h, *m_gfx);
    m_nodeThumbs.clear();
}

// ==== UI ====
void MaterialEditorWindow::DrawToolbar(const MaterialEditorContext& ctx, Tab& t)
{
    MaterialEditSession& s = *t.session;
    const int tab = m_active;
    if (ImGui::Button("Save")) m_pendingSaves.push_back({ tab, {}, false, 0, false });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write %s (Ctrl+S is the scene's)", s.Path().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Save As..."))
    {
        const bool inst = s.IsInstance();
        static const char kImat[]     = "Material (*.imat)\0*.imat\0All Files\0*.*\0\0";
        static const char kImatInst[] = "Material Instance (*.imatinst)\0*.imatinst\0All Files\0*.*\0\0";
        const std::string path = SaveFileDialog(inst ? kImatInst : kImat, inst ? "imatinst" : "imat", "asset/");
        if (!path.empty()) m_pendingSaves.push_back({ tab, path, false, 0, false });
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as Instance..."))
    {
        static const char kImatInst[] = "Material Instance (*.imatinst)\0*.imatinst\0All Files\0*.*\0\0";
        const std::string path = SaveFileDialog(kImatInst, "imatinst", "asset/");
        if (!path.empty()) m_pendingSaves.push_back({ tab, path, true, 0, false });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A new .imatinst whose parent is this file (as saved): what you\n"
                          "changed here becomes its overrides. Opens in a new tab.");
    ImGui::SameLine(0.f, 16.f);
    ImGui::BeginDisabled(s.UndoDepth() == 0);
    if (ImGui::Button("Undo")) RequestUndo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(s.RedoDepth() == 0);
    if (ImGui::Button("Redo")) RequestRedo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) m_pendingRevert.push_back(tab);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the file again (one undo step)");
    ImGui::SameLine(0.f, 16.f);
    if (ctx.reveal && ImGui::Button("Browse")) ctx.reveal(s.Path());
    // A standalone material without a graph can get one (Convert to Graph); a hand-written
    // custom shader cannot, it stays code.
    if (!t.graph && !s.IsInstance() && s.Mat().materialGraph.empty())
    {
        const bool customHlsl = s.Mat().useCustomShader && !s.Mat().customShaderPath.empty();
        ImGui::SameLine(0.f, 16.f);
        ImGui::BeginDisabled(customHlsl);
        if (ImGui::Button("Create Graph"))
        {
            std::string why;
            if (!CreateGraph("standard", why)) LOG_WARNING("[MATGRAPH] create: %s", why.c_str());
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(customHlsl
                ? "A hand-written custom shader stays code: edit it with Open .hlsl (Details);\n"
                  "its parameters come from the shader's reflection."
                : "Convert this material to a node graph: a Standard Surface node (the material as it\n"
                  "renders now) wired to the Output. Save writes the graph, its generated shader and\n"
                  "the material.");
    }
    if (TabDirty(t))
    {
        ImGui::SameLine(0.f, 16.f);
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "(unsaved)");
    }
}

void MaterialEditorWindow::DrawDetails(const MaterialEditorContext& ctx, Tab& t)
{
    MaterialEditSession& s = *t.session;
    const MaterialComponent& m = s.Mat();
    ImGui::SeparatorText("Details");
    ImGui::TextDisabled("Kind");
    ImGui::SameLine(80.f);
    static const char* kKind[] = { "Standard", "Custom HLSL", "Graph", "Instance", "Entity" };
    ImGui::TextUnformatted(t.graph && s.Kind() != MaterialKind::Graph ? "Graph (not saved yet)"
                                                                       : kKind[static_cast<int>(s.Kind())]);
    if (!t.graphError.empty())
        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "Its graph did not open: %s", t.graphError.c_str());
    if (t.graph)
    {
        // The graph and what it compiles to (the canvas draws it).
        const MaterialGraph::CompileResult& r = t.graph->Compiled(ActiveGraphOptions());
        ImGui::TextDisabled("Graph");
        ImGui::SameLine(80.f);
        ImGui::TextWrapped("%s%s", t.graph->Path().c_str(), t.graph->Dirty() ? " *" : "");
        ImGui::TextDisabled("Nodes");
        ImGui::SameLine(80.f);
        ImGui::Text("%zu, %zu links, %d params, %d textures", t.graph->Graph().nodes.size(), t.graph->Graph().links.size(),
                    r.stats.params, r.stats.textures);
        int errors = 0, warnings = 0;
        for (const MaterialGraph::Diagnostic& d : r.diagnostics)
        {
            if (d.severity == MaterialGraph::Severity::Error) ++errors;
            else if (d.severity == MaterialGraph::Severity::Warning) ++warnings;
        }
        ImGui::TextDisabled("Compile");
        ImGui::SameLine(80.f);
        if (r.ok) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.f), "ok, %d bytes, %d warning(s)", r.stats.hlslBytes, warnings);
        else      ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%d error(s) - Save refuses", errors);
        // Stats: the vertex stage, and the shader pool every compiled file uses for good.
        ImGui::TextDisabled("Stats");
        ImGui::SameLine(80.f);
        ImGui::Text("VS %s, cbuffer %d B, shader pool %zu / %zu", r.stats.usesVertex ? "yes (World Position Offset)" : "no",
                    r.stats.cbufferBytes, ctx.renderer ? ctx.renderer->ShaderPoolUsed() : std::size_t(0),
                    ctx.renderer ? ctx.renderer->ShaderPoolCapacity() : std::size_t(0));
        for (const MaterialGraph::Diagnostic& d : r.diagnostics)
        {
            if (d.severity == MaterialGraph::Severity::Info) continue;
            ImGui::TextColored(d.severity == MaterialGraph::Severity::Error ? ImVec4(1.f, 0.45f, 0.4f, 1.f)
                                                                              : ImVec4(0.9f, 0.75f, 0.3f, 1.f),
                               "%s #%u%s%s: %s", MaterialGraph::SeverityName(d.severity), d.node, d.pin.empty() ? "" : ".",
                               d.pin.c_str(), d.message.c_str());
        }
    }
    if (s.IsEntity())
    {
        // One entity's own material. Its edits are its MaterialOverride (what the scene saves),
        // against its asset.
        ImGui::TextDisabled("Entity");
        ImGui::SameLine(80.f);
        ImGui::TextUnformatted(t.entityName.c_str());
        ImGui::TextDisabled("Asset");
        ImGui::SameLine(80.f);
        if (s.Info().parentPath.empty())
            ImGui::TextDisabled("none - an inline material (the scene saves it whole)");
        else
        {
            if (ImGui::SmallButton(BaseName(s.Info().parentPath).c_str())) RequestOpen(s.Info().parentPath);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nOpen the asset in a tab of its own", s.Info().parentPath.c_str());
        }
        ImGui::TextDisabled("Overrides");
        ImGui::SameLine(80.f);
        ImGui::Text("%zu", s.Info().overrideKeys.size());
        ImGui::TextDisabled("Custom shader parameters and textures cannot be per-entity overrides.");
        return;
    }
    ImGui::TextDisabled("File");
    ImGui::SameLine(80.f);
    ImGui::TextWrapped("%s", s.Path().c_str());
    if (s.IsInstance())
    {
        // The parent chain, each a click away (it opens in a tab of its own).
        ImGui::TextDisabled("Parent");
        std::string p = s.Info().parentPath;
        for (int depth = 0; !p.empty() && depth < 8; ++depth)
        {
            if (depth == 0)
                ImGui::SameLine(80.f);
            else
            {
                ImGui::Dummy(ImVec2(80.f - ImGui::GetStyle().ItemSpacing.x, 0.f));
                ImGui::SameLine();
                ImGui::TextDisabled("of");
                ImGui::SameLine();
            }
            ImGui::PushID(depth);
            if (ImGui::SmallButton(BaseName(p).c_str())) RequestOpen(p);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nOpen it in a tab of its own", p.c_str());
            ImGui::PopID();
            p = Resource::GetMaterialParent(p);
        }
        if (!s.Info().parentResolved)
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "The parent chain does not resolve.");
        ImGui::TextDisabled("Overrides");
        ImGui::SameLine(80.f);
        ImGui::Text("%zu", s.Info().overrideKeys.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset all"))
        {
            std::string why;
            if (!s.ResetOverrides(why)) LOG_WARNING("[MATEDITOR] reset overrides: %s", why.c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Follow the parent for everything (one undo step)");
        if (!s.Info().explicitFormat)
            ImGui::TextColored(ImVec4(0.85f, 0.7f, 0.25f, 1.f), "legacy value-diff file: Save writes it explicit");
    }
    if (m.useCustomShader && !m.customShaderPath.empty())
    {
        ImGui::SeparatorText("Shader");
        ImGui::TextWrapped("%s", m.customShaderPath.c_str());
        const int ps = ctx.renderer ? ctx.renderer->LookupMaterialCustomPSID(m) : -1;
        if (ps < 0)
            ImGui::TextDisabled("not resolved yet (no material has used it)");
        else
        {
            const std::string err = ctx.renderer->GetCustomShaderError(ps);
            if (err.empty())
                ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.f), "compiled (id %d)", ps);
            else
            {
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "compile error:");
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextUnformatted(err.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        if (ImGui::SmallButton("Open .hlsl"))
        {
            const std::wstring w = Sys::Utf8ToWide(Sys::PathToUtf8(std::filesystem::absolute(
                Sys::U8Path(m.customShaderPath))));
            ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open the shader in the system's editor");

        // What rays and shadows see of it: neither runs this shader.
        ImGui::TextDisabled("Rays");
        ImGui::SameLine(80.f);
        ImGui::TextWrapped("%s", RtProxySource(m, ctx.renderer).c_str());
        if (!s.IsEntity())
        {
            bool rtStd = m.rtUseStandardMaterial;
            if (ImGui::Checkbox("Rays use the engine slots", &rtStd))
            {
                std::string why;
                if (!s.Set({ { "rtUseStandardMaterial", rtStd ? "1" : "0" } }, why))
                    LOG_WARNING("[MATEDITOR] rtUseStandardMaterial: %s", why.c_str());
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("rtUseStandardMaterial: reflections, GI and RT shadows read the Base Color, Normal,\n"
                                  "Surface and Emissive slots below instead of guessing from parameter names.");
        }
        if (ShadowMaskGap(m, t.graph.get()))
        {
            ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.25f, 1.f), "Shadows: the whole polygon.");
            ImGui::TextWrapped("The shadow pass clips by OPACITYMAP.r x BASECOLORMAP.a, and neither holds a mask.");
            const std::string mirror = t.graph ? MirrorMaskPath(t.graph->Compiled(GraphOptions(t)), m) : std::string();
            ImGui::BeginDisabled(mirror.empty() || s.IsEntity());
            if (ImGui::SmallButton("Mirror mask to OPACITYMAP"))
            {
                std::string why;
                if (!s.Set({ { "tex_OPACITYMAP", mirror } }, why)) LOG_WARNING("[MATEDITOR] mirror mask: %s", why.c_str());
                else LOG_INFO("[MATGRAPH] mirror mask %s: tex_OPACITYMAP = %s", s.Path().c_str(), mirror.c_str());
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(mirror.empty()
                    ? "The mask is not one texture's R channel: bind a mask to the Opacity Mask slot by hand."
                    : "Bind %s to OPACITYMAP (one undo step): the shadow pass clips by its R.", mirror.c_str());
        }
    }
}

void MaterialEditorWindow::DrawAskModal()
{
    const char* id = "Unsaved material edits##mateditor";
    if (m_askOpenPopup) { ImGui::OpenPopup(id); m_askOpenPopup = false; }
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const bool window = m_askFor == -2;
    if (window) ImGui::Text("Some materials have unsaved edits.");
    else if (m_askFor >= 0 && m_askFor < static_cast<int>(m_tabs.size()))
        ImGui::Text("'%s' has unsaved edits.", m_tabs[m_askFor].session->Path().c_str());
    ImGui::TextDisabled(window ? "Save them before closing the Material Editor?" : "Save them before closing the tab?");
    if (ImGui::Button("Save"))
    {
        if (window)
        {
            for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
                if (TabDirty(m_tabs[i])) m_pendingSaves.push_back({ i, {}, false, 0, false });
            m_open = false;   // PPA closes the tabs once they are clean
        }
        else
            m_pendingSaves.push_back({ m_askFor, {}, false, 0, true });
        m_askFor = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard"))
    {
        std::string why;
        if (window) { CloseAll(true, why); m_open = false; }
        else CloseTab(m_askFor, true, why);
        m_askFor = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        m_askFor = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MaterialEditorWindow::Draw(const MaterialEditorContext& ctx)
{
    if (!m_open) return;
    ImGui::SetNextWindowSize(ImVec2(1240.f, 820.f), ImGuiCond_FirstUseEver);
    if (m_pinRect)
    {
        // -mateditor_rect: the same place and size every run (captures compare pixels).
        ImGui::SetNextWindowPos(ImVec2(m_rect[0], m_rect[1]), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(m_rect[2], m_rect[3]), ImGuiCond_Always);
    }
    bool keep = true;
    const bool visible = ImGui::Begin("Material Editor###MaterialEditor", &keep, ImGuiWindowFlags_NoScrollbar);
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!keep) RequestClose();
    if (!visible)
    {
        ImGui::End();
        return;
    }
    auto acceptDrop = [this]()
    {
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IMAT_PATH"))
                RequestOpen(std::string(static_cast<const char*>(p->Data)));
            ImGui::EndDragDropTarget();
        }
    };
    if (m_tabs.empty())
    {
        ImGui::TextDisabled("Right-click a material in the Content Browser > Open in Material Editor, or drag one here.");
        ImGui::Dummy(ImGui::GetContentRegionAvail());
        acceptDrop();
        DrawAskModal();
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##mattabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
    {
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        {
            bool tabKeep = true;
            const std::string label = m_tabs[i].name + (TabDirty(m_tabs[i]) ? " *" : "") + "###mattab" +
                                      m_tabs[i].session->Path() +
                                      (m_tabs[i].session->IsEntity() ? "#" + std::to_string(m_tabs[i].entity.entity)
                                                                     : std::string());
            const ImGuiTabItemFlags f = (i == m_focusTab) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), &tabKeep, f))
            {
                if (m_focusTab < 0 || m_focusTab == i) m_active = i;
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_tabs[i].session->Path().c_str());
            if (!tabKeep)
            {
                // -uitest_reverse=material.tab_close_asks: the x discards the edits without asking.
                const bool ask = TabDirty(m_tabs[i]) && !UITests::Reverted(UITests::Fix::TabCloseAsks);
                if (ask) { if (m_askFor == -1) { m_askFor = i; m_askOpenPopup = true; } }
                else
                {
                    std::string why;
                    CloseTab(i, true, why);
                    break;   // the vector changed under the loop
                }
            }
        }
        m_focusTab = -1;
        ImGui::EndTabBar();
    }
    Tab* t = ActiveTabPtr();
    if (!t)
    {
        DrawAskModal();
        ImGui::End();
        return;
    }
    MaterialEditSession& s = *t->session;

    if (s.IsEntity()) DrawEntityToolbar(ctx, *t);
    else              DrawToolbar(ctx, *t);
    if (t->diskChanged)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.8f, 0.4f, 1.f));
        ImGui::TextWrapped("This file (or a material it inherits from) changed on disk while you had unsaved edits.");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload")) m_pendingRevert.push_back(m_active);
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep mine")) t->diskChanged = false;
    }
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // Wide enough for the preview's largest quantised size (512) beside a scrollbar.
    const float  leftW = (std::min)(548.f, (std::max)(300.f, avail.x * 0.45f));
    ImGui::BeginChild("##matleft", ImVec2(leftW, 0.f), ImGuiChildFlags_None);
    m_preview->Draw(ctx.renderer, s.Path(), PreviewMaterial(*t));   // a live graph's shader
    DrawDetails(ctx, *t);
    if (t->graph && t->canvas) t->canvas->DrawNodePanel(*t->graph);   // the selected node's props
    ImGui::EndChild();
    acceptDrop();
    ImGui::SameLine();
    ImGui::BeginChild("##matpanel", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders);
    if (t->graph && !m_canvasOff) DrawGraphPanes(ctx, *t);
    else                          DrawMaterialPanel(ctx, *t);
    ImGui::EndChild();
    // A value dragged over many frames is ONE step of the graph's ring, and Ctrl+Z over the
    // canvas steps that ring (elsewhere the material's).
    if (t->graph) t->graph->EndFrame(ImGui::IsAnyItemActive());
    m_undoToGraph = t->graph && t->canvas && t->canvas->Hovered();
    DrawAskModal();
    ImGui::End();
}

void MaterialEditorWindow::DrawMaterialPanel(const MaterialEditorContext& ctx, Tab& t)
{
    MaterialEditSession& s = *t.session;
    MaterialPanelContext pc;
    pc.renderer  = ctx.renderer;
    pc.world     = ctx.world;
    pc.entity    = NullEntity;
    pc.assetMode = true;
    pc.assetInfo = &s.Info();
    bool panelDirty = false;
    pc.assetDirty = &panelDirty;
    pc.onEdit     = [&s](const char* key) { s.MarkEdited(key); };
    s.BeginFrame();
    m_panel.Draw(s.Mat(), pc);
    s.EndFrame(ImGui::IsAnyItemActive());
    // An entity tab's edit goes into the entity now (its MaterialOverride).
    if (s.IsEntity()) FlushEntity(ctx, t);
    DrawSwitches(t);
}

MaterialGraphDocument* MaterialEditorWindow::InstanceGraph(Tab& t)
{
    MaterialEditSession& s = *t.session;
    if (!s.IsInstance() || s.Mat().materialGraph.empty()) return nullptr;
    if (!t.parentGraph || t.parentGraph->Path() != s.Mat().materialGraph)
    {
        auto g = std::make_unique<MaterialGraphDocument>();
        t.parentGraphError.clear();
        if (!g->Open(s.Mat().materialGraph, t.parentGraphError)) { t.parentGraph.reset(); return nullptr; }
        t.parentGraph = std::move(g);
    }
    return t.parentGraph.get();
}

MaterialGraph::CompileOptions MaterialEditorWindow::InstanceGraphOptions(const Tab& t) const
{
    // The parent's graph as THIS instance compiles it: its shading model, blend mode
    // and switches (inherited or overridden); the graph's path in the header.
    MaterialGraph::CompileOptions o;
    o.shadingModel = t.session->Mat().customShadingModel;
    o.blendMode    = t.session->Mat().userBlendMode;
    o.sourceLabel  = t.session->Mat().materialGraph;
    for (const auto& [name, on] : t.session->Mat().staticSwitches) o.switches[name] = on;
    return o;
}

void MaterialEditorWindow::DrawSwitches(Tab& t)
{
    MaterialEditSession& s = *t.session;
    if (s.IsFunction() || s.IsEntity()) return;
    MaterialGraphDocument* g = t.graph ? t.graph.get() : InstanceGraph(t);
    if (!g) return;
    const MaterialGraph::CompileResult& r = g->Compiled(t.graph ? GraphOptions(t) : InstanceGraphOptions(t));
    if (r.switches.empty()) return;
    ImGui::SeparatorText("Static Switches");
    ImGui::TextDisabled(t.graph ? "Compile-time: each combination is its own shader (Save writes it)."
                                : "Compile-time: an instance that changes one gets its own shader (Save writes it).");
    for (const MaterialGraph::StaticSwitchInfo& w : r.switches)
    {
        ImGui::PushID(w.name.c_str());
        const std::string key = "sswitch_" + w.name;
        std::string why;
        if (s.IsInstance())
        {
            bool over = s.Info().overrideKeys.count(key) != 0;
            if (ImGui::Checkbox("##over", &over) && !s.SetOverride(key, over, why))
                LOG_WARNING("[MATGRAPH] switch %s: %s", w.name.c_str(), why.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Override the parent's value");
            ImGui::SameLine();
        }
        bool v = w.value;
        if (ImGui::Checkbox(w.name.c_str(), &v) && !s.Set({ { key, v ? "1" : "0" } }, why))
            LOG_WARNING("[MATGRAPH] switch %s: %s", w.name.c_str(), why.c_str());
        if (w.value != w.def)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(default %s)", w.def ? "on" : "off");
        }
        ImGui::PopID();
    }
}

bool MaterialEditorWindow::ApplyInstanceSwitches(Tab& t, std::string& why)
{
    // An instance whose combination is not its parent's needs that combination's
    // shader: compiled from the parent's graph, written beside it, named by this
    // instance's customShaderPath (an override). Its own combination = its parent's:
    // nothing to write, and no override needed.
    MaterialEditSession& s = *t.session;
    MaterialGraphDocument* g = InstanceGraph(t);
    if (!g) return true;
    const MaterialGraph::CompileResult& r = g->Compiled(InstanceGraphOptions(t));
    // A graph without an owner also names a file by the shading model / blend mode, which an
    // instance may override: it can need its own file with no switches at all.
    const std::string materialKey = MaterialKey(g->Graph(), s.Mat());
    if (r.switches.empty() && materialKey.empty()) return true;
    if (!r.ok) { why = "graph_errors: the parent's graph does not compile with this instance's switches"; return false; }
    const std::string hlsl = ShaderPathForCombination(g->Path(), r.switches, materialKey);
    if (s.Mat().customShaderPath == hlsl && Sys::FileExists(hlsl)) return true;
    if (hlsl != ShaderPathForGraph(g->Path()) && !Sys::FileExists(hlsl) && CombinationFiles(g->Path()) >= kMaxSwitchCombinations)
    {
        why = "switch_combinations: '" + g->Path() + "' has " + std::to_string(kMaxSwitchCombinations) +
              " static switch combinations already";
        return false;
    }
    std::string current;
    if (!(Sys::ReadFileText(hlsl, current) && WithoutCR(current) == r.hlsl))
    {
        Sys::CreateParentDirs(hlsl);
        if (!Sys::WriteFileBytesAtomic(hlsl, r.hlsl.data(), r.hlsl.size())) { why = "cannot write '" + hlsl + "'"; return false; }
        if (!Watched(hlsl)) HotReload::RequestShaderReload(hlsl);
    }
    if (s.Mat().customShaderPath != hlsl && !s.Set({ { "customShaderPath", hlsl } }, why)) return false;
    LOG_INFO("[MATGRAPH] instance %s: switches -> %s", s.Path().c_str(), hlsl.c_str());
    return true;
}

void MaterialEditorWindow::DrawGraphPanes(const MaterialEditorContext& ctx, Tab& t)
{
    if (!t.canvas)
    {
        t.canvas = std::make_unique<MaterialGraphCanvas>();
        t.canvas->SetThumbnailSource([this](const std::string& p) { return NodeThumbnail(p); });
    }
    MaterialGraphDocument& g = *t.graph;
    const MaterialGraph::CompileResult& compiled = g.Compiled(ActiveGraphOptions());
    // DXC's errors on the live shader join the compiler's, on their nodes. A mask the shadow
    // pass never sees is a warning on the Output node.
    const bool shadowGap = !t.session->IsFunction() && ShadowMaskGap(t.session->Mat(), &g);
    MaterialGraph::CompileResult withDxc;
    if (!t.graphLive.dxc.empty() || shadowGap)
    {
        withDxc = compiled;
        withDxc.diagnostics.insert(withDxc.diagnostics.end(), t.graphLive.dxc.begin(), t.graphLive.dxc.end());
        if (shadowGap)
        {
            MaterialGraph::Diagnostic d;
            d.node     = g.OutputId();
            d.severity = MaterialGraph::Severity::Warning;
            d.code     = "masked_shadow";
            d.message  = "shadows clip by the material's OPACITYMAP.r x BASECOLORMAP.a, not this mask - with neither set "
                         "the shadow is the whole polygon (Details: Mirror mask to OPACITYMAP)";
            withDxc.diagnostics.push_back(std::move(d));
        }
    }
    const MaterialGraph::CompileResult& r = t.graphLive.dxc.empty() && !shadowGap ? compiled : withDxc;
    if (!ImGui::BeginTabBar("##graphpanes")) return;
    if (ImGui::BeginTabItem("Graph", nullptr, t.selectGraphPane ? ImGuiTabItemFlags_SetSelected : 0))
    {
        t.graphPane = 0;
        // The graph's own ring (the toolbar's Undo / Redo are the material's).
        ImGui::BeginDisabled(g.UndoDepth() == 0);
        if (ImGui::SmallButton("Undo##graph")) g.Undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(g.RedoDepth() == 0);
        if (ImGui::SmallButton("Redo##graph")) g.Redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) t.canvas->RequestFit();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame the whole graph (Home); F frames the selection");
        ImGui::SameLine(0.f, 16.f);
        int errors = 0, warnings = 0;
        for (const MaterialGraph::Diagnostic& d : r.diagnostics)
        {
            if (d.severity == MaterialGraph::Severity::Error) ++errors;
            else if (d.severity == MaterialGraph::Severity::Warning) ++warnings;
        }
        if (r.ok) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.f), "compiles: %d bytes, %d params, %d textures%s",
                                     r.stats.hlslBytes, r.stats.params, r.stats.textures,
                                     warnings ? (", " + std::to_string(warnings) + " warning(s)").c_str() : "");
        else      ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%d error(s): Save refuses (hover a red node)", errors);
        // What the preview shows: the live (unsaved) graph, or the saved shader.
        ImGui::SameLine(0.f, 16.f);
        const GraphLive& L = t.graphLive;
        if (L.job.valid())               ImGui::TextColored(ImVec4(0.85f, 0.8f, 0.4f, 1.f), "live: compiling...");
        else if (!L.dxc.empty())         ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "live: DXC error (the preview keeps the last good one)");
        else if (L.active)               ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.f), "live: previewing (%.0f ms)", L.lastMs);
        else                             ImGui::TextDisabled("live: the saved shader");
        // The previewed node, Find, Hide Unrelated.
        if (t.previewNode)
        {
            ImGui::SameLine(0.f, 16.f);
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.f, 1.f), "previewing node #%u .%s", t.previewNode, t.previewPin.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop##preview"))
            {
                std::string why;
                SetPreviewOn(t, 0, "", why);
            }
        }
        ImGui::SetNextItemWidth(160.0f);
        const bool enter = ImGui::InputTextWithHint("##find", "Find (name, type, #id)", t.find, sizeof(t.find),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        const std::vector<std::uint32_t> found = g.Find(t.find);
        if (t.find[0])
        {
            ImGui::SameLine();
            if (found.empty()) ImGui::TextDisabled("no match");
            else ImGui::TextDisabled("%d/%d", t.findAt >= 0 ? t.findAt + 1 : 0, static_cast<int>(found.size()));
        }
        if (enter && !found.empty())
        {
            // Enter: the next match, selected and framed.
            t.findAt = (t.findAt + 1) % static_cast<int>(found.size());
            t.canvas->RequestSelect({ found[static_cast<std::size_t>(t.findAt)] });
            t.canvas->RequestFrameSelection();
        }
        if (!t.find[0]) t.findAt = -1;
        ImGui::SameLine(0.f, 16.f);
        bool hide = t.canvas->HideUnrelated();
        if (ImGui::Checkbox("Hide unrelated", &hide)) t.canvas->SetHideUnrelated(hide);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dim the nodes the selection neither reads nor feeds");
        ImGui::SameLine(0.f, 16.f);
        ImGui::TextDisabled("right-click: add a node / preview one");
        ImGui::BeginChild("##canvas", ImVec2(0.f, 0.f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        t.canvas->SetPreviewNode(t.previewNode);
        t.canvas->Draw(g, r, g.Path());
        ImGui::EndChild();
        {
            std::uint32_t node = 0;
            std::string pin, why;
            if (t.canvas->TakePreviewRequest(node, pin) && !SetPreviewOn(t, node, pin, why))
                LOG_WARNING("[MATGRAPH] preview %s: %s", g.Path().c_str(), why.c_str());
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Parameters"))
    {
        t.graphPane = 3;
        DrawParameters(t);
        ImGui::EndTabItem();
    }
    if (!t.session->IsFunction() && ImGui::BeginTabItem("Material"))   // a function has no material
    {
        t.graphPane = 1;
        ImGui::TextDisabled("The material's settings: shading model and blend mode choose the Output's pins.");
        DrawMaterialPanel(ctx, t);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Code"))
    {
        t.graphPane = 2;
        DrawCode(t);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    t.selectGraphPane = false;
}

// Every parameter of the graph, one row per name (same-named nodes are one parameter),
// renamed, grouped, sorted, ranged and given its default in place. Each change is set on
// every node of that name (MaterialGraphDocument::SetParameter: one undo step; a drag
// merges through EndFrame). The material's VALUES are its cparam_ / ctex_ / sswitch_ keys:
// the Material tab.
void MaterialEditorWindow::DrawParameters(Tab& t)
{
    MaterialGraphDocument& g = *t.graph;
    const std::vector<MaterialGraphDocument::ParameterRow> rows = g.Parameters();
    if (rows.empty())
    {
        ImGui::TextDisabled("No parameters. Add a Scalar / Vector / Texture Sample Parameter or a Static Switch Parameter.");
        return;
    }
    ImGui::TextDisabled("The graph's parameters and their defaults; the material's values are on the Material tab.");
    auto set = [&](const std::string& name, const char* key, MaterialGraph::Value v)
    {
        MaterialGraph::ValueMap props;
        props[key] = std::move(v);
        std::string why;
        if (!g.SetParameter(name, props, why)) LOG_WARNING("[MATGRAPH] parameter %s: %s", name.c_str(), why.c_str());
    };
    auto text = [&](const char* id, const std::string& cur, const std::function<void(const std::string&)>& commit)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if ((ImGui::InputText(id, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) ||
             ImGui::IsItemDeactivatedAfterEdit()) && cur != buf)
            commit(buf);
    };
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##params", 6, kFlags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Sort", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableHeadersRow();
    for (const MaterialGraphDocument::ParameterRow& r : rows)
    {
        const MaterialGraph::Node& n = *r.first;
        const std::string name = r.name;
        ImGui::PushID((r.type + "/" + r.name).c_str());
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        text("##name", name, [&](const std::string& v) { set(name, "name", MaterialGraph::Value::String(v)); });

        ImGui::TableSetColumnIndex(1);
        const char* kind = r.type == "ScalarParameter" ? "Scalar" : r.type == "VectorParameter" ? "Vector"
                         : r.type == "TextureSampleParameter2D" ? "Texture" : "Static Switch";
        std::string ids;
        for (const std::uint32_t id : r.nodes) ids += (ids.empty() ? "#" : " #") + std::to_string(id);
        if (ImGui::SmallButton(kind))
        {
            // Find it on the canvas: every node of the name, selected and framed.
            t.canvas->RequestSelect(r.nodes);
            t.canvas->RequestFrameSelection();
            t.selectGraphPane = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s): select on the canvas", ids.c_str(), r.type.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-1.0f);
        if (r.type == "ScalarParameter")
        {
            float v = static_cast<float>(n.PropNumber("default", 0.0));
            if (ImGui::DragFloat("##def", &v, 0.01f)) set(name, "default", MaterialGraph::Value::Number(v));
        }
        else if (r.type == "VectorParameter")
        {
            float v[4] = { 1, 1, 1, 1 };
            if (const MaterialGraph::Value* d = n.Prop("default"))
                for (int i = 0; i < 4; ++i) v[i] = static_cast<float>(d->Components() == 1 ? d->At(0) : d->At(i));
            if (ImGui::ColorEdit4("##def", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                set(name, "default", MaterialGraph::Value::Numbers({ v[0], v[1], v[2], v[3] }));
        }
        else if (r.type == "TextureSampleParameter2D")
        {
            text("##def", n.PropString("default"), [&](const std::string& v) { set(name, "default", MaterialGraph::Value::String(v)); });
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                    set(name, "default", MaterialGraph::Value::String(std::string(static_cast<const char*>(pl->Data))));
                ImGui::EndDragDropTarget();
            }
        }
        else
        {
            bool v = n.PropBool("default", true);
            if (ImGui::Checkbox("##def", &v)) set(name, "default", MaterialGraph::Value::Bool(v));
        }

        ImGui::TableSetColumnIndex(3);
        if (r.type == "ScalarParameter")
        {
            const MaterialGraph::Value* lo = n.Prop("min");
            const MaterialGraph::Value* hi = n.Prop("max");
            if (lo || hi)
            {
                float mm[2] = { lo ? static_cast<float>(lo->At(0)) : 0.0f, hi ? static_cast<float>(hi->At(0)) : 1.0f };
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat2("##range", mm, 0.01f))
                {
                    MaterialGraph::ValueMap props;
                    props["min"] = MaterialGraph::Value::Number(mm[0]);
                    props["max"] = MaterialGraph::Value::Number(mm[1]);
                    std::string why;
                    if (!g.SetParameter(name, props, why)) LOG_WARNING("[MATGRAPH] parameter %s: %s", name.c_str(), why.c_str());
                }
            }
            else if (ImGui::SmallButton("+ range"))
            {
                MaterialGraph::ValueMap props;
                props["min"] = MaterialGraph::Value::Number(0.0);
                props["max"] = MaterialGraph::Value::Number(1.0);
                std::string why;
                if (!g.SetParameter(name, props, why)) LOG_WARNING("[MATGRAPH] parameter %s: %s", name.c_str(), why.c_str());
            }
        }
        else if (r.type == "TextureSampleParameter2D")
        {
            static const char* kSamplers[] = { "Color", "Linear", "Normal", "Mask", "Grayscale" };
            const std::string cur = n.PropString("sampler", "Color");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##sampler", cur.c_str()))
            {
                for (const char* sname : kSamplers)
                    if (ImGui::Selectable(sname, cur == sname)) set(name, "sampler", MaterialGraph::Value::String(sname));
                ImGui::EndCombo();
            }
        }
        else ImGui::TextDisabled("-");

        ImGui::TableSetColumnIndex(4);
        text("##group", n.PropString("group"), [&](const std::string& v) { set(name, "group", MaterialGraph::Value::String(v)); });

        ImGui::TableSetColumnIndex(5);
        float sort = static_cast<float>(n.PropNumber("sort", 0.0));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("##sort", &sort, 1.0f, 0.0f, 0.0f, "%g")) set(name, "sort", MaterialGraph::Value::Number(sort));
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void MaterialEditorWindow::DrawCode(Tab& t)
{
    const MaterialGraph::CompileResult& r = t.graph->Compiled(ActiveGraphOptions());
    if (!r.ok)
    {
        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "The graph does not compile:");
        for (const MaterialGraph::Diagnostic& d : r.diagnostics)
            if (d.severity == MaterialGraph::Severity::Error)
            {
                ImGui::PushID(&d);
                if (ImGui::Selectable(("#" + std::to_string(d.node) + " " + d.message).c_str()) && d.node)
                {
                    t.canvas->RequestSelect({ d.node });
                    t.canvas->RequestFrameSelection();
                    t.selectGraphPane = true;
                }
                ImGui::PopID();
            }
        return;
    }
    ImGui::TextDisabled("The generated shader (%d bytes). Click a line: its node.", r.stats.hlslBytes);
    ImGui::BeginChild("##code", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    std::vector<std::pair<std::size_t, std::size_t>> lines;   // [begin, end) of each line
    for (std::size_t b = 0; b < r.hlsl.size();)
    {
        const std::size_t e = r.hlsl.find('\n', b);
        lines.emplace_back(b, e == std::string::npos ? r.hlsl.size() : e);
        b = e == std::string::npos ? r.hlsl.size() : e + 1;
    }
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(lines.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
        {
            const std::string line = r.hlsl.substr(lines[static_cast<std::size_t>(i)].first,
                                                   lines[static_cast<std::size_t>(i)].second - lines[static_cast<std::size_t>(i)].first);
            ImGui::PushID(i);
            if (ImGui::Selectable(line.empty() ? " " : line.c_str()))
            {
                // "// @n<id>" ends every line a node emitted.
                const std::size_t at = line.rfind("// @n");
                if (at != std::string::npos)
                {
                    const std::uint32_t id = static_cast<std::uint32_t>(std::strtoul(line.c_str() + at + 5, nullptr, 10));
                    if (id)
                    {
                        t.canvas->RequestSelect({ id });
                        t.canvas->RequestFrameSelection();
                        t.selectGraphPane = true;
                    }
                }
            }
            ImGui::PopID();
        }
    ImGui::EndChild();
}

bool MaterialEditorWindow::GraphView(bool fit, bool frameSelection, const std::vector<std::uint32_t>* select)
{
    Tab* t = ActiveTabPtr();
    if (!t || !t->graph) return false;
    if (!t->canvas) t->canvas = std::make_unique<MaterialGraphCanvas>();
    if (select) t->canvas->RequestSelect(*select);
    if (fit) t->canvas->RequestFit();
    if (frameSelection) t->canvas->RequestFrameSelection();
    t->selectGraphPane = true;
    return true;
}

std::vector<std::uint32_t> MaterialEditorWindow::GraphSelection() const
{
    if (m_active < 0 || m_active >= static_cast<int>(m_tabs.size()) || !m_tabs[m_active].canvas) return {};
    return m_tabs[m_active].canvas->Selection();
}

void MaterialEditorWindow::Shutdown()
{
    for (Tab& t : m_tabs)
    {
        if (t.canvas) t.canvas->Shutdown();
        if (t.graphLive.job.valid()) t.graphLive.job.wait();   // it reads the renderer's library
    }
}

// ==== Harness ====
bool MaterialEditorWindow::HarnessConfigure(std::string& why)
{
    const auto& cl = Sys::CommandLine::Get();
    static const char* kSteps[] = {
        "-mateditor_size", "-mateditor_set", "-mateditor_override", "-mateditor_view", "-mateditor_save",
        "-mateditor_save_instance", "-mateditor_reopen", "-mateditor_dump", "-mateditor_undo", "-mateditor_redo",
        "-mateditor_revert", "-mateditor_close_at", "-mateditor_assign", "-mateditor_activate",
        "-matgraph_create", "-matgraph_ops", "-matgraph_apply", "-matgraph_view", "-matgraph_preview",
    };
    if (cl.Has("-mateditor_rect"))
    {
        // x,y,w,h: the window pinned there (a capture compares pixels).
        const std::vector<std::string> v = SplitList(cl.Get("-mateditor_rect"), ',');
        bool ok = v.size() == 4;
        for (int i = 0; ok && i < 4; ++i)
        {
            ok = IsFrame(v[static_cast<std::size_t>(i)]);
            if (ok) m_rect[i] = static_cast<float>(std::atoi(v[static_cast<std::size_t>(i)].c_str()));
        }
        if (!ok || m_rect[2] < 400.f || m_rect[3] < 300.f)
        {
            why = "-mateditor_rect=<x>,<y>,<w>,<h> in pixels (w >= 400, h >= 300)";
            return false;
        }
        m_pinRect = true;
    }
    // The canvas pack's reverse arm: a graph tab shows the material panel, no canvas.
    m_canvasOff = cl.Has("-matgraph_canvas_off");
    const bool hasSubject = cl.Has("-mateditor_open") || cl.Has("-mateditor_scratch") || cl.Has("-mateditor_entity");
    for (const char* f : kSteps)
        if (cl.Has(f) && !hasSubject)
        {
            why = std::string(f) + " without -mateditor_open=<material> (or -mateditor_scratch, "
                  "-mateditor_entity) has no subject";
            return false;
        }
    if (!hasSubject) return true;
    for (const char* f : { "-mateditor_open", "-mateditor_scratch" })
        if (cl.Has(f))
        {
            const std::string v = cl.Get(f);
            if (v.empty()) { why = std::string(f) + " needs a material path"; return false; }
            for (const std::string& item : SplitList(v, ','))
            {
                std::string path = Resource::AssetRefs::PercentDecode(item);
                if (std::string(f) == "-mateditor_scratch")
                {
                    // A copy under the run's diagnostics folder: the pack edits and
                    // saves it, the tracked fixture is never touched.
                    const std::string dst = Sys::DiagnosticsDir() + "/mateditor/" + BaseName(path);
                    std::error_code ec;
                    std::filesystem::create_directories(Sys::U8Path(dst).parent_path(), ec);
                    std::filesystem::copy_file(Sys::U8Path(path), Sys::U8Path(dst),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) { why = "-mateditor_scratch: cannot copy '" + path + "' (" + ec.message() + ")"; return false; }
                    path = dst;
                    // A graph material's copy owns a copy of its graph and that copy's shader, so a pack that
                    // edits and saves it never writes the tracked originals (Save writes the shader beside the
                    // graph). The shader is compiled from the copy, not copied: its header names the graph,
                    // and the tab would otherwise see a change to preview.
                    MaterialComponent m;
                    if (Resource::LoadMaterial(dst, m) && !m.materialGraph.empty())
                    {
                        const std::string dir = Sys::DiagnosticsDir() + "/mateditor/";
                        const std::string graph = Canon(dir + BaseName(m.materialGraph));
                        std::filesystem::copy_file(Sys::U8Path(m.materialGraph), Sys::U8Path(graph),
                                                   std::filesystem::copy_options::overwrite_existing, ec);
                        if (ec) { why = "-mateditor_scratch: cannot copy the graph of '" + path + "' (" + ec.message() + ")"; return false; }
                        MaterialGraph::Graph gr;
                        std::string err;
                        if (!MaterialGraph::LoadGraph(graph, gr, err)) { why = "-mateditor_scratch: " + err; return false; }
                        MaterialGraph::CompileOptions o;
                        o.shadingModel = m.customShadingModel;
                        o.blendMode    = m.userBlendMode;
                        o.sourceLabel  = graph;
                        for (const auto& [name, on] : m.staticSwitches) o.switches[name] = on;   // its combination
                        const MaterialGraph::CompileResult r = MaterialGraph::Compile(gr, o);
                        const std::string hlsl = ShaderPathForCombination(graph, r.switches, MaterialKey(gr, m));
                        Sys::CreateParentDirs(hlsl);
                        if (!r.ok || !Sys::WriteFileBytesAtomic(hlsl, r.hlsl.data(), r.hlsl.size()))
                        {
                            why = "-mateditor_scratch: cannot compile '" + graph + "' into '" + hlsl + "'";
                            return false;
                        }
                        m.materialGraph    = graph;
                        m.customShaderPath = hlsl;
                        if (!Resource::SaveMaterial(m, dst)) { why = "-mateditor_scratch: cannot rewrite '" + dst + "'"; return false; }
                    }
                }
                m_harnessOpen.push_back(path);
            }
        }
    if (cl.Has("-mateditor_size"))
    {
        const std::string v = cl.Get("-mateditor_size");
        const std::uint32_t px = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        if (px != 128 && px != 256 && px != 384 && px != 512)
        {
            why = "-mateditor_size is 128, 256, 384 or 512 (the preview's quantised sizes)";
            return false;
        }
        m_preview->SetHarnessSize(px);
    }
    auto list = [&](const char* flag, int kind, bool frameOnly) -> bool
    {
        if (!cl.Has(flag)) return true;
        const std::string v = cl.Get(flag);
        if (v.empty()) { why = std::string(flag) + " needs a value"; return false; }
        for (const std::string& item : SplitList(v, ','))
        {
            Step s;
            s.kind = kind;
            if (frameOnly)
            {
                if (!IsFrame(item)) { why = std::string(flag) + ": '" + item + "' is not an editor frame"; return false; }
                s.at = std::strtoull(item.c_str(), nullptr, 10);
            }
            else if (!SplitAt(item, s.text, s.at) || s.text.empty())
            {
                why = std::string(flag) + ": '" + item + "' is not <value>@<frame>";
                return false;
            }
            m_steps.push_back(std::move(s));
        }
        return true;
    };
    if (!list("-mateditor_set", kStepSet, false) || !list("-mateditor_override", kStepOverride, false) ||
        !list("-mateditor_view", kStepView, false) || !list("-mateditor_save", kStepSave, true) ||
        !list("-mateditor_save_instance", kStepSaveInstance, false) || !list("-mateditor_reopen", kStepReopen, true) ||
        !list("-mateditor_dump", kStepDump, false) || !list("-mateditor_undo", kStepUndo, true) ||
        !list("-mateditor_redo", kStepRedo, true) || !list("-mateditor_revert", kStepRevert, true) ||
        !list("-mateditor_close_at", kStepClose, true) || !list("-mateditor_assign", kStepAssign, false) ||
        !list("-mateditor_entity", kStepEntity, false) || !list("-mateditor_activate", kStepActivate, false) ||
        !list("-matgraph_create", kStepGraphCreate, false) || !list("-matgraph_ops", kStepGraphOps, false) ||
        !list("-matgraph_apply", kStepSave, true) || !list("-matgraph_view", kStepGraphView, false) ||
        !list("-matgraph_preview", kStepGraphPreview, false))
        return false;
    for (Step& s : m_steps)
    {
        if (s.kind == kStepGraphCreate && s.text != "standard" && s.text != "empty")
        {
            why = "-matgraph_create: '" + s.text + "' is not standard|empty@<frame>";
            return false;
        }
        if (s.kind == kStepGraphView && s.text != "fit" && s.text != "selection" && s.text.rfind("select:", 0) != 0 &&
            s.text != "unrelated:on" && s.text != "unrelated:off")
        {
            why = "-matgraph_view: '" + s.text + "' is not fit|selection|select:<id>[+<id>...]|unrelated:on|unrelated:off@<frame>";
            return false;
        }
        if (s.kind == kStepGraphPreview && s.text != "off" && !IsFrame(s.text.substr(0, s.text.find(':'))))
        {
            why = "-matgraph_preview: '" + s.text + "' is not <node id>[:<output pin>]|off@<frame>";
            return false;
        }
        if (s.kind == kStepGraphOps)
        {
            // Read and parse now: a bad file is exit 12 before a frame, not a failure at frame N.
            std::string text;
            if (!Sys::ReadFileText(s.text, text)) { why = "-matgraph_ops: cannot read '" + s.text + "'"; return false; }
            std::vector<GraphOp> ops;
            std::string err;
            if (!ParseGraphOps(text, ops, err)) { why = "-matgraph_ops: '" + s.text + "': " + err; return false; }
            s.text = std::move(text);
        }
    }
    for (const Step& s : m_steps)
        if (s.kind == kStepActivate && !IsFrame(s.text))
        {
            why = "-mateditor_activate: '" + s.text + "' is not a tab index (0-based)@<frame>";
            return false;
        }
    for (const Step& s : m_steps)
    {
        std::vector<std::pair<std::string, std::string>> kv;
        if (s.kind == kStepSet && !ParseAssignments(s.text, kv))
        {
            why = "-mateditor_set: '" + s.text + "' is not key=value[;key=value]";
            return false;
        }
        if (s.kind == kStepOverride)
        {
            const std::size_t c = s.text.rfind(':');
            if (c == std::string::npos || c == 0 || (s.text.substr(c + 1) != "0" && s.text.substr(c + 1) != "1"))
            {
                why = "-mateditor_override: '" + s.text + "' is not <key>:<0|1>";
                return false;
            }
        }
        if (s.kind == kStepView)
        {
            std::uint32_t mode = 0;
            if (!MeshEditorWindow::ParseViewMode(Lower(s.text), mode))
            {
                why = "-mateditor_view: '" + s.text + "' is not a view mode (lit, unlit, wireframe, basecolor, ...)";
                return false;
            }
        }
    }
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
    LOG_INFO("[MATEDITOR] harness: open=%zu steps=%zu autoOverrideOff=%d", m_harnessOpen.size(), m_steps.size(),
             cl.Has("-mateditor_autooverride_off") ? 1 : 0);
    return true;
}

void MaterialEditorWindow::TickHarness(const MaterialEditorContext& ctx)
{
    if ((m_harnessOpen.empty() && m_steps.empty()) || m_harnessFailed) return;
    if (!m_harnessOpened)
    {
        m_harnessOpened = true;
        for (const std::string& p : m_harnessOpen)
        {
            std::string why;
            if (!OpenNow(p, why))
            {
                m_harnessFailed = true;
                LOG_ERROR("-mateditor_open='%s' cannot open the Material Editor: %s", p.c_str(), why.c_str());
                return;
            }
        }
        if (!m_harnessOpen.empty()) Activate(0);
    }
    for (Step& st : m_steps)
    {
        if (st.done || ctx.editorFrame < st.at) continue;
        st.done = true;
        MaterialEditSession* s = ActiveSession();
        if (!s && st.kind != kStepClose && st.kind != kStepEntity)
        {
            m_harnessFailed = true;
            LOG_ERROR("[MATEDITOR] harness step at editor frame %llu: no tab is open",
                      static_cast<unsigned long long>(st.at));
            return;
        }
        std::string why;
        bool ok = true;
        switch (st.kind)
        {
        case kStepSet:
        {
            std::vector<std::pair<std::string, std::string>> kv;
            ParseAssignments(st.text, kv);
            ok = s->Set(kv, why);
            if (ok)
                LOG_INFO("[MATEDITOR] set %s: %s overrides=%zu", s->Path().c_str(), st.text.c_str(),
                         s->Info().overrideKeys.size());
            break;
        }
        case kStepOverride:
        {
            const std::size_t c = st.text.rfind(':');
            ok = s->SetOverride(st.text.substr(0, c), st.text.substr(c + 1) == "1", why);
            if (ok)
                LOG_INFO("[MATEDITOR] override %s: %s overrides=%zu", s->Path().c_str(), st.text.c_str(),
                         s->Info().overrideKeys.size());
            break;
        }
        case kStepView:
            ok = SetViewMode(st.text, why);
            break;
        case kStepSave:
            m_pendingSaves.push_back({ m_active, {}, false, 0, false });
            break;
        case kStepSaveInstance:
            m_pendingSaves.push_back({ m_active, HarnessPath(st.text), true, 0, false });
            break;
        case kStepReopen:
        {
            // Close (discarding) and read the file again: what was SAVED.
            const std::string path = s->Path();
            ok = CloseTab(m_active, true, why) && OpenNow(path, why);
            break;
        }
        case kStepDump:
            LogDump(*s, st.text);
            break;
        case kStepUndo:
            ok = s->Undo();
            if (!ok) why = "nothing to undo";
            else LOG_INFO("[MATEDITOR] undo %s: undo=%zu redo=%zu dirty=%d", s->Path().c_str(), s->UndoDepth(),
                          s->RedoDepth(), s->Dirty() ? 1 : 0);
            break;
        case kStepRedo:
            ok = s->Redo();
            if (!ok) why = "nothing to redo";
            else LOG_INFO("[MATEDITOR] redo %s: undo=%zu redo=%zu dirty=%d", s->Path().c_str(), s->UndoDepth(),
                          s->RedoDepth(), s->Dirty() ? 1 : 0);
            break;
        case kStepRevert:
            ok = RevertActive(why);
            break;
        case kStepClose:
            CloseAll(true, why);
            m_open = false;
            break;
        case kStepAssign:
            // The live-overlay pack: a scene entity reads the tab's file (the scratch copy), so the
            // live overlay has someone to show the edits to.
            ok = ctx.assign && ctx.assign(st.text, s->Path(), why);
            if (!ok && why.empty()) why = "no editor world";
            if (ok) LOG_INFO("[MATEDITOR] assign %s -> %s", st.text.c_str(), s->Path().c_str());
            break;
        case kStepEntity:
        {
            // The named entity's own material in a tab (exactly one entity has the name).
            ok = false;
            if (!ctx.world) { why = "no editor world"; break; }
            Entity found = NullEntity;
            int named = 0;
            for (Entity c : ctx.world->GetEntities())
                if (ctx.world->IsAlive(c) && ctx.world->GetName(c) == st.text) { found = c; ++named; }
            if (named != 1) { why = std::to_string(named) + " entities are named '" + st.text + "'"; break; }
            ok = OpenEntityNow(*ctx.world, found, why);
            break;
        }
        case kStepActivate:
            ok = Activate(std::atoi(st.text.c_str()));
            if (!ok) why = "no tab " + st.text;
            else LOG_INFO("[MATEDITOR] activate %s: %s", st.text.c_str(), ActiveSession()->Path().c_str());
            break;
        case kStepGraphCreate:
            ok = CreateGraph(st.text, why);
            break;
        case kStepGraphView:
        {
            std::vector<std::uint32_t> ids;
            if (st.text.rfind("select:", 0) == 0)
                for (const std::string& id : SplitList(st.text.substr(7), '+'))
                    ids.push_back(static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 10)));
            if (st.text == "unrelated:on" || st.text == "unrelated:off")
                ok = SetGraphHideUnrelated(st.text == "unrelated:on");
            else
                ok = GraphView(st.text == "fit", st.text == "selection" || !ids.empty(), ids.empty() ? nullptr : &ids);
            if (!ok) why = "the active tab has no graph";
            else LOG_INFO("[MATGRAPH] view %s", st.text.c_str());
            break;
        }
        case kStepGraphPreview:
        {
            const std::size_t colon = st.text.find(':');
            const std::uint32_t node = st.text == "off" ? 0u : static_cast<std::uint32_t>(std::strtoul(st.text.c_str(), nullptr, 10));
            ok = SetGraphPreview(node, colon == std::string::npos ? std::string() : st.text.substr(colon + 1), why);
            break;
        }
        case kStepGraphOps:
        {
            MaterialGraphDocument* g = ActiveGraph();
            if (!g) { ok = false; why = "the active tab has no graph (-matgraph_create)"; break; }
            std::vector<GraphOp> ops;
            std::vector<std::uint32_t> ids;
            ok = ParseGraphOps(st.text, ops, why) && g->Apply(ops, &ids, why);
            if (ok)
                LOG_INFO("[MATGRAPH] ops %s: %zu op(s) new=%zu revision=%llu nodes=%zu links=%zu", g->Path().c_str(),
                         ops.size(), ids.size(), static_cast<unsigned long long>(g->Revision()), g->Graph().nodes.size(),
                         g->Graph().links.size());
            break;
        }
        default:
            break;
        }
        if (!ok)
        {
            m_harnessFailed = true;
            LOG_ERROR("[MATEDITOR] harness step '%s' at editor frame %llu: %s", st.text.c_str(),
                      static_cast<unsigned long long>(st.at), why.c_str());
            return;
        }
    }
}
}
