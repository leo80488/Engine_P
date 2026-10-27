// SkeletonEditorWindow.cpp - see the header for the contract.

#include "Editor/SkeletonEditorWindow.h"

#include "ECS/AnimationComponents.h"            // SkeletonComponent::poseByteOffset (the stage's pin read-out)
#include "ECS/AnimEntityResolve.h"              // the stage's Animator owner
#include "ECS/AnimationSystem.h"                // the selected bone's animated local pose
#include "ECS/FollowComponents.h"               // a preview attachment follows its socket
#include "ECS/Components.h"                     // MaterialComponent (RequestPreview's payload)
#include "ECS/LocomotionComponents.h"           // BoneMaskMode (the mask preview)
#include "Editor/DrawListRaster.h"
#include "Editor/HumanoidMapping.h"             // the role rows, the .iskel's humanoid section
#include "Editor/PreviewPanelMath.h"            // QuantiseBox / BuildCam (the Mesh Editor's)
#include "Graphics/IGraphicsDevice.h"           // CaptureTextureToPNG (-skeleton_editor_shot)
#include "Graphics/MeshDescriptorHeap.h"        // the weight heatmap's own slots
#include "Resource/AnimationLoader.h"           // the animation list reads .ianim as playback does
#include "Resource/AnimationResource.h"
#include "Resource/AssetFS.h"
#include "Math/MathUtils.h"                     // the engine's Euler order
#include "Renderer/Preview/PreviewStudioRig.h"  // kStudioCamera
#include "Renderer/Preview/PreviewTypes.h"
#include "Renderer/Renderer.h"
#include "Resource/AssetRefs.h"                 // PercentDecode
#include "Resource/HumanoidBone.h"
#include "Resource/HumanoidProfileBuilder.h"   // Auto-Detect / a hand edit's derived fields
#include "Resource/SkeletonAsset.h"
#include "Scene/SceneInstanceLoader.h"          // LoadSkeletonFile / ModelSkeletonPath
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"                // ImGui::GetCurrentWindow (ImGuizmo's hover window)
#include "imgui/ImGuizmo/ImGuizmo.h"             // the socket gizmo

#include <DirectXMath.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>   // the per-user preview-attachment list

using namespace DirectX;

namespace Editor
{
namespace
{
    constexpr double        kResizeDebounceSeconds = 0.200;
    constexpr float         kPickRadiusPx  = 12.f;    // the chain preview's rule
    constexpr std::uint32_t kMaxOverlayLines = 40000;
    constexpr std::uint64_t kSettleFrames  = 4;       // a shot waits for a picture this old
    constexpr std::uint64_t kShotDeadline  = 400;     // editor frames
    constexpr std::size_t   kUndoDepth     = 64;
    constexpr int           kSocketGizmoId = 0x534B5347;   // 'SKSG' (ImGuizmo::PushID)
    constexpr const char*   kSocketPayload = "SKE_SOCKET";  // a socket row dragged onto a bone

    enum StepKind : int { kStepSelect = 1, kStepScope, kStepShow, kStepOrbit, kStepShot, kStepOverlayShot, kStepClose };

    struct ScopeEntry { SkeletonEditorWindow::BoneScope scope; const char* name; const char* label; };
    constexpr ScopeEntry kScopes[] = {
        { SkeletonEditorWindow::BoneScope::None,                "none",     "None" },
        { SkeletonEditorWindow::BoneScope::Selected,            "selected", "Selected Only" },
        { SkeletonEditorWindow::BoneScope::SelectedAndParents,  "parents",  "Selected and Parents" },
        { SkeletonEditorWindow::BoneScope::SelectedAndChildren, "children", "Selected and Children" },
        { SkeletonEditorWindow::BoneScope::Family,              "family",   "Selected, Parents and Children" },
        { SkeletonEditorWindow::BoneScope::All,                 "all",      "All Hierarchy" },
    };

    struct ShowEntry { std::uint32_t bit; const char* name; const char* label; };
    constexpr ShowEntry kShows[] = {
        { SkeletonEditorWindow::kShowJoints,     "joints",     "Joints" },
        { SkeletonEditorWindow::kShowOctahedra,  "octahedra",  "Octahedral bones" },
        { SkeletonEditorWindow::kShowNames,      "names",      "Selected bone name" },
        { SkeletonEditorWindow::kShowAllNames,   "allnames",   "All bone names" },
        { SkeletonEditorWindow::kShowAxes,       "axes",       "Selected bone axes" },
        { SkeletonEditorWindow::kShowAllAxes,    "allaxes",    "All bone axes" },
        { SkeletonEditorWindow::kShowCategories, "categories", "Colour by category" },
        { SkeletonEditorWindow::kShowGrid,       "grid",       "Ground grid" },
        { SkeletonEditorWindow::kShowSockets,    "sockets",    "Sockets" },
        { SkeletonEditorWindow::kShowVirtual,    "virtualbones", "Virtual bones" },
        { SkeletonEditorWindow::kShowGhost,      "ghost",      "Reference pose ghost (Animation host)" },
        { SkeletonEditorWindow::kShowHumanoid,   "humanoid",   "Humanoid roles (always in the Retarget tab)" },
    };

    struct TabEntry { SkeletonEditorWindow::Tab tab; const char* name; };
    constexpr TabEntry kTabs[] = {
        { SkeletonEditorWindow::Tab::Details,    "details" },
        { SkeletonEditorWindow::Tab::Check,      "check" },
        { SkeletonEditorWindow::Tab::Animations, "animations" },
        { SkeletonEditorWindow::Tab::Retarget,   "retarget" },
    };

    // The mask preview's modes: -1 off, else BoneMaskMode.
    struct MaskEntry { int mode; const char* name; const char* label; };
    constexpr MaskEntry kMasks[] = {
        { -1, "off",    "Off" },
        { static_cast<int>(BoneMaskMode::FullBody),   "full",   "Full Body" },
        { static_cast<int>(BoneMaskMode::UpperBody),  "upper",  "Upper Body (humanoid spine subtree)" },
        { static_cast<int>(BoneMaskMode::LowerBody),  "lower",  "Lower Body (the rest)" },
        { static_cast<int>(BoneMaskMode::CustomRoot), "custom", "Custom Root (a bone and its subtree)" },
    };

    struct HostEntry { SkeletonEditorWindow::Host host; const char* name; const char* label; };
    constexpr HostEntry kHosts[] = {
        { SkeletonEditorWindow::Host::Reference, "reference", "Reference Pose" },
        { SkeletonEditorWindow::Host::Stage,     "stage",     "Animation (live stage)" },
    };

    struct FilterEntry { SkeletonEditorWindow::TreeFilter f; const char* name; const char* label; };
    constexpr FilterEntry kFilters[] = {
        { SkeletonEditorWindow::TreeFilter::All,      "all",      "All bones" },
        { SkeletonEditorWindow::TreeFilter::Weighted, "weighted", "Weighted bones" },
        { SkeletonEditorWindow::TreeFilter::Deform,   "deform",   "Deform (weighted or 付与 target)" },
        { SkeletonEditorWindow::TreeFilter::IK,       "ik",       "IK (bone / target / link)" },
        { SkeletonEditorWindow::TreeFilter::Twist,    "twist",    "Twist bones" },
        { SkeletonEditorWindow::TreeFilter::Chain,    "chain",    "Chain physics (by name)" },
        { SkeletonEditorWindow::TreeFilter::Humanoid, "humanoid", "Humanoid roles" },
        { SkeletonEditorWindow::TreeFilter::Issues,   "issues",   "Bones with issues" },
    };

    // Colours.
    constexpr ImU32 kColBone       = IM_COL32(200, 205, 220, 210);
    constexpr ImU32 kColSelected   = IM_COL32(255, 205, 60, 255);
    constexpr ImU32 kColAncestor   = IM_COL32(110, 200, 255, 235);
    constexpr ImU32 kColDescendant = IM_COL32(255, 150, 70, 235);
    constexpr ImU32 kColIKBone     = IM_COL32(255, 80, 80, 235);
    constexpr ImU32 kColIKTarget   = IM_COL32(255, 125, 170, 235);
    constexpr ImU32 kColIKLink     = IM_COL32(230, 100, 230, 235);
    constexpr ImU32 kColGrant      = IM_COL32(80, 220, 220, 235);
    constexpr ImU32 kColTwist      = IM_COL32(120, 230, 120, 235);
    constexpr ImU32 kColChain      = IM_COL32(100, 150, 255, 235);
    constexpr ImU32 kColDummy      = IM_COL32(130, 130, 130, 190);
    constexpr ImU32 kColSocket     = IM_COL32(70, 240, 190, 255);
    constexpr ImVec4 kColSocketText = ImVec4(0.45f, 0.95f, 0.78f, 1.f);
    constexpr ImU32 kColVirtual    = IM_COL32(205, 130, 255, 255);    // a virtual bone
    constexpr ImVec4 kColVirtualText = ImVec4(0.82f, 0.6f, 1.f, 1.f);
    constexpr ImVec4 kColWarnText  = ImVec4(1.f, 0.72f, 0.3f, 1.f);
    constexpr ImU32 kColMaskIn     = IM_COL32(90, 230, 120, 245);     // a bone the mask drives
    constexpr ImU32 kColMaskOut    = IM_COL32(120, 120, 130, 120);    // a bone the mask leaves alone
    constexpr ImU32 kColRole       = IM_COL32(240, 225, 120, 255);    // a humanoid role's marker
    constexpr const char* kVbPickPopup = "##ske_vb_targets_popup";   // the Details' "+ Add Virtual Bone"

    // The projection of the model's composed space onto a picture rect: the
    // studio fit (composed -> the rig's unit box, Renderer::GetPreviewAssetFit)
    // then the camera the renderer builds for this view.
    struct Projection
    {
        XMMATRIX mvp;
        float    x = 0.f, y = 0.f, w = 1.f, h = 1.f;
        float    scale = 1.f;   // composed units -> unit box
        bool Project(const float* p, ImVec2& out) const
        {
            const XMVECTOR c = XMVector4Transform(XMVectorSet(p[0], p[1], p[2], 1.f), mvp);
            const float cw = XMVectorGetW(c);
            if (cw <= 1e-6f) return false;
            const float nx = XMVectorGetX(c) / cw, ny = XMVectorGetY(c) / cw;
            out = ImVec2(x + (nx * 0.5f + 0.5f) * w, y + (1.f - (ny * 0.5f + 0.5f)) * h);
            return true;
        }
    };

    std::string LowerAscii(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    // A shot's log prefix names its file: the runner's logLineFields reads the
    // last line of a prefix, so one prefix per shot lets a pack bound each.
    std::string BaseName(const std::string& p)
    {
        const std::size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    }

    // A harness file name: relative = under the run's diagnostics folder.
    std::string HarnessPath(const std::string& p)
    {
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\');
        return absolute ? p : Sys::DiagnosticsDir() + "/" + p;
    }

    // "<value>@<frame>" -> value, frame. No '@' = frame 0.
    bool SplitAt(const std::string& item, std::string& value, std::uint64_t& at)
    {
        const std::size_t p = item.find_last_of('@');
        if (p == std::string::npos) { value = item; at = 0; return !value.empty(); }
        value = item.substr(0, p);
        const std::string f = item.substr(p + 1);
        if (value.empty() || f.empty() ||
            !std::all_of(f.begin(), f.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
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
            const std::string item = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
            if (!item.empty()) out.push_back(item);
            if (e == std::string::npos) break;
            b = e + 1;
        }
        return out;
    }

    bool EndsWithCI(const std::string& s, const char* ext)
    {
        const std::string l = LowerAscii(s);
        const std::size_t n = std::strlen(ext);
        return l.size() >= n && l.compare(l.size() - n, n, ext) == 0;
    }

    // A file's size + write time ("" = unreadable), to tell whether it changed.
    std::string FileStamp(const std::string& path)
    {
        std::error_code ec;
        // The file the loaders read: through the search roots, a worktree's
        // shared content root included.
        const std::string onDisk = Resource::AssetFS::Get().ResolveOnDisk(path);
        const auto p    = Sys::U8Path(onDisk.empty() ? path : onDisk);
        const auto size = std::filesystem::file_size(p, ec);
        if (ec) return {};
        const auto when = std::filesystem::last_write_time(p, ec);
        if (ec) return {};
        return std::to_string(size) + ":" + std::to_string(when.time_since_epoch().count());
    }

    // Why @p path cannot be written here ("" = it can): the Mesh Editor's rule.
    std::string FileLock(const std::string& path, const char* what)
    {
        std::error_code ec;
        // A file only a worktree's shared content root has is writable: a save writes
        // this checkout's copy of the path, which then wins over the main checkout's
        // (Resource/SharedContent.h). Anything else absent here is the pak's.
        const auto& afs = Resource::AssetFS::Get();
        const bool sharedOnly = afs.ResolveLocal(path).empty() && afs.IsSharedPath(afs.ResolveOnDisk(path));
        if (!sharedOnly && !std::filesystem::is_regular_file(Sys::U8Path(path), ec))
            return std::string("the ") + what + " is not a loose file (packed): read-only";
        if (Sys::ThroughReparsePoint(path))
            return std::string("the ") + what + " is another checkout's (reached through a junction): read-only here";
        return {};
    }

    // The panel's records are the model's skeleton sockets: a Mesh's (`SO mesh=`,
    // the Mesh Editor's) are neither shown, staged nor written here, so a Mesh
    // Editor save does not leave this panel dirty.
    std::vector<Resource::ModelSocket> SkeletonSocketsOf(const std::vector<Resource::ModelSocket>& all)
    {
        std::vector<Resource::ModelSocket> out;
        out.reserve(all.size());
        for (const Resource::ModelSocket& s : all)
            if (s.IsSkeletonSocket()) out.push_back(s);
        return out;
    }

    bool MakeProjection(Renderer* renderer, const std::string& model, float yaw, float pitch, float dist,
                        const float pan[3], float x, float y, float w, float h, Projection& out)
    {
        if (!renderer || model.empty() || w <= 0.f || h <= 0.f) return false;
        float centre[3], scale = 1.f;
        if (!renderer->GetPreviewAssetFit(model, centre, scale)) return false;   // not composed yet
        const PreviewPanel::Cam cam = PreviewPanel::BuildCam(yaw, pitch, dist, pan, w / h);
        const XMMATRIX fit = XMMatrixMultiply(XMMatrixTranslation(-centre[0], -centre[1], -centre[2]),
                                              XMMatrixScaling(scale, scale, scale));
        out.mvp   = XMMatrixMultiply(fit, cam.viewProj);
        out.x     = x;
        out.y     = y;
        out.w     = w;
        out.h     = h;
        out.scale = scale;
        return true;
    }

    // A Projection from a frame-space -> clip matrix (SkeletonEditorWindow::FrameFor).
    Projection ProjectionOf(const XMFLOAT4X4& toClip, float x, float y, float w, float h)
    {
        Projection p;
        p.mvp = XMLoadFloat4x4(&toClip);
        p.x   = x;
        p.y   = y;
        p.w   = w;
        p.h   = h;
        return p;
    }
}

// ==== Names ====
const char* SkeletonEditorWindow::BoneScopeName(BoneScope s)
{
    for (const ScopeEntry& e : kScopes)
        if (e.scope == s) return e.name;
    return "?";
}

bool SkeletonEditorWindow::ParseBoneScope(const std::string& s, BoneScope& out)
{
    const std::string n = LowerAscii(s);
    for (const ScopeEntry& e : kScopes)
        if (n == e.name) { out = e.scope; return true; }
    return false;
}

bool SkeletonEditorWindow::ParseShow(const std::string& text, std::uint32_t& out)
{
    const std::string t = LowerAscii(text);
    if (t == "none" || t == "0") { out = 0; return true; }
    if (t == "all") { out = kShowAll; return true; }
    if (t.size() > 2 && t[0] == '0' && t[1] == 'x')
    {
        char* end = nullptr;
        const unsigned long v = std::strtoul(t.c_str() + 2, &end, 16);
        if (!end || *end) return false;
        out = static_cast<std::uint32_t>(v) & kShowAll;
        return true;
    }
    std::uint32_t bits = 0;
    for (const std::string& part : SplitList(t, '+'))
    {
        bool found = false;
        for (const ShowEntry& e : kShows)
            if (part == e.name) { bits |= e.bit; found = true; }
        if (!found) return false;
    }
    out = bits;
    return true;
}

std::string SkeletonEditorWindow::ShowNames(std::uint32_t bits)
{
    std::string s;
    for (const ShowEntry& e : kShows)
        if (bits & e.bit) s += (s.empty() ? "" : "+") + std::string(e.name);
    return s.empty() ? std::string("none") : s;
}

const char* SkeletonEditorWindow::HostName(Host h)
{
    for (const HostEntry& e : kHosts)
        if (e.host == h) return e.name;
    return "?";
}

bool SkeletonEditorWindow::ParseHost(const std::string& s, Host& out)
{
    const std::string n = LowerAscii(s);
    for (const HostEntry& e : kHosts)
        if (n == e.name) { out = e.host; return true; }
    return false;
}

const char* SkeletonEditorWindow::TreeFilterName(TreeFilter f)
{
    for (const FilterEntry& e : kFilters)
        if (e.f == f) return e.name;
    return "?";
}

bool SkeletonEditorWindow::ParseTreeFilter(const std::string& s, TreeFilter& out)
{
    const std::string n = LowerAscii(s);
    for (const FilterEntry& e : kFilters)
        if (n == e.name) { out = e.f; return true; }
    return false;
}

const char* SkeletonEditorWindow::TabName(Tab t)
{
    for (const TabEntry& e : kTabs)
        if (e.tab == t) return e.name;
    return "?";
}

bool SkeletonEditorWindow::ParseTab(const std::string& s, Tab& out)
{
    const std::string n = LowerAscii(s);
    for (const TabEntry& e : kTabs)
        if (n == e.name) { out = e.tab; return true; }
    return false;
}

const char* SkeletonEditorWindow::MaskModeName(int mode)
{
    for (const MaskEntry& e : kMasks)
        if (e.mode == mode) return e.name;
    return "?";
}

bool SkeletonEditorWindow::ParseMaskMode(const std::string& s, int& out)
{
    const std::string n = LowerAscii(s);
    for (const MaskEntry& e : kMasks)
        if (n == e.name) { out = e.mode; return true; }
    return false;
}

// ==== Lifetime ====
// The .iskel's blend streams, kept for ReInspect (a humanoid edit).
struct SkeletonEditorWindow::WeightStreams
{
    std::vector<SceneInstanceLoader::SkinBlendStream> blends;
};

SkeletonEditorWindow::SkeletonEditorWindow()  = default;
SkeletonEditorWindow::~SkeletonEditorWindow() = default;

SkeletonEditorWindow& SkeletonEditorWindow::Get()
{
    static SkeletonEditorWindow s_instance;
    return s_instance;
}

void SkeletonEditorWindow::RequestOpen(const std::string& path)
{
    m_open       = true;
    m_openQueued = true;
    m_queuedPath = path;
    m_focusQueued = true;   // to the front: a docked panel behind another tab comes forward
}

bool SkeletonEditorWindow::OpenNow(const SkeletonEditorContext& ctx, const std::string& path, std::string& code,
                                   std::string& why, bool discard)
{
    if (Dirty() && !discard)
    {
        code = "dirty";
        why  = "the socket / virtual bone / humanoid edits of '" + (m_model.empty() ? m_iskel : m_model) +
               "' are not saved - save them "
               "(Editor.SkeletonEditorSave) or open with { discard = true }";
        return false;
    }
    m_open = true;
    if (!LoadSubject(ctx, path, code, why))
    {
        m_error = why;
        return false;
    }
    return true;
}

// ==== The subject ====
bool SkeletonEditorWindow::LoadSubject(const SkeletonEditorContext& ctx, const std::string& pathIn, std::string& code,
                                       std::string& why)
{
    const std::string path = Sys::RelativizeAssetPath(pathIn);
    std::string model, iskel;
    if (EndsWithCI(path, ".iscn"))
    {
        model = path;
        iskel = SceneInstanceLoader::ModelSkeletonPath(model);
        if (iskel.empty())
        {
            code = "unsupported";
            why  = "'" + path + "' names no skeleton (no K line): it is not a skinned model";
            return false;
        }
    }
    else if (EndsWithCI(path, ".iskel"))
    {
        iskel = path;
        // The picture: the same-stem model beside it, when its K line names
        // this very file.
        const std::filesystem::path p = Sys::U8Path(path);
        const std::string sibling = Sys::RelativizeAssetPath(
            Sys::PathToUtf8(p.parent_path() / Sys::U8Path(Sys::PathToUtf8(p.stem()) + ".iscn")));
        std::error_code ec;
        if (std::filesystem::is_regular_file(Sys::U8Path(sibling), ec))
        {
            const std::string k = SceneInstanceLoader::ModelSkeletonPath(sibling);
            if (!k.empty() && LowerAscii(Sys::RelativizeAssetPath(k)) == LowerAscii(iskel)) model = sibling;
        }
    }
    else
    {
        code = "bad_value";
        why  = "the Skeleton Editor opens a model (.iscn) or a skeleton (.iskel): '" + path + "'";
        return false;
    }

    auto skel = std::make_unique<SkeletonAsset>();
    std::vector<ClipAsset> clips;
    std::vector<SceneInstanceLoader::SkinBlendStream> blends;
    if (!SceneInstanceLoader::LoadSkeletonFile(iskel, *skel, clips, &blends))
    {
        code = "load_failed";
        why  = "the skeleton '" + iskel + "' did not load";
        return false;
    }
    const std::string humanoidSource = Resource::EnsureHumanoidProfile(*skel);
    std::vector<Resource::SkinWeightStream> streams;
    streams.reserve(blends.size());
    for (const auto& b : blends) streams.push_back({ b.weights.data(), b.weights.size() });
    Resource::SkeletonReport report = Resource::InspectSkeleton(*skel, streams);

    // Bind pose, the model's space: inverse(inverseBindPose) - bone -> the space
    // the vertices were bound in (the Mesh Editor's skeleton overlay), which is
    // the space a Model preview composes the skinned parts in.
    const std::uint32_t n = std::min(skel->boneCount, SkeletonAsset::MAX_BONES);
    std::vector<float> joints(std::size_t(n) * 3), axes(std::size_t(n) * 9);
    std::vector<std::vector<int>> children(n);
    std::vector<std::string> names(n), lower(n);
    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (std::uint32_t b = 0; b < n; ++b)
    {
        XMVECTOR det;
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixInverse(&det, XMLoadFloat4x4(&skel->inverseBindPose[b])));
        joints[b * 3 + 0] = m._41;
        joints[b * 3 + 1] = m._42;
        joints[b * 3 + 2] = m._43;
        const float rows[3][3] = { { m._11, m._12, m._13 }, { m._21, m._22, m._23 }, { m._31, m._32, m._33 } };
        for (int r = 0; r < 3; ++r)
        {
            const float len = std::sqrt(rows[r][0] * rows[r][0] + rows[r][1] * rows[r][1] + rows[r][2] * rows[r][2]);
            for (int c = 0; c < 3; ++c) axes[b * 9 + r * 3 + c] = len > 1e-8f ? rows[r][c] / len : (r == c ? 1.f : 0.f);
        }
        for (int a = 0; a < 3; ++a)
        {
            lo[a] = std::min(lo[a], joints[b * 3 + a]);
            hi[a] = std::max(hi[a], joints[b * 3 + a]);
        }
        const int p = skel->parentIndex[b];
        if (p >= 0 && p < static_cast<int>(n) && p != static_cast<int>(b)) children[p].push_back(static_cast<int>(b));
        names[b] = std::string(skel->boneNames[b], strnlen(skel->boneNames[b], sizeof(skel->boneNames[b])));
        lower[b] = LowerAscii(names[b]);
    }
    std::vector<std::vector<int>> boneIssues(n);
    for (std::size_t i = 0; i < report.issues.size(); ++i)
        if (report.issues[i].bone >= 0 && report.issues[i].bone < static_cast<int>(n))
            boneIssues[report.issues[i].bone].push_back(static_cast<int>(i));
    // Bone name -> index (the first bone of a name, as the runtime resolves).
    std::vector<std::pair<std::string, int>> byName;
    byName.reserve(n);
    for (std::uint32_t b = 0; b < n; ++b) byName.emplace_back(names[b], static_cast<int>(b));
    std::stable_sort(byName.begin(), byName.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // The model's socket and virtual bone records: the model file holds them (an
    // .iskel alone: none).
    std::vector<Resource::ModelSocket> sockets;
    std::vector<Resource::ModelVirtualBone> vbs;
    if (!model.empty())
    {
        sockets = SkeletonSocketsOf(Resource::ModelSocketsAt(model)->sockets);
        vbs     = Resource::ModelVirtualBonesAt(model)->bones;
    }

    // ---- Commit ----
    // A different picture: the old composition goes back.
    if (m_model != model && !m_model.empty() && ctx.renderer) ctx.renderer->InvalidatePreviewAsset(m_model);
    // The stage was the old subject's (this runs in the game half: a Lua call or
    // the queued open in ProcessPendingActions).
    if (m_stage.Up() || m_stage.View() != ~0u) TearDownStage(ctx, "subject changed");
    m_stageRefusedFor.clear();
    m_stageStatus.clear();
    // A clip picked for another skeleton is not this one's (the list is rescanned).
    if (m_model != model && !m_clipPlayer.Path().empty()) m_clipPlayer.Select(ctx.animClipSys, "");
    m_clipRequested = false;
    // The weight heatmap pairs the model's skinned parts with these blend streams
    // (the old copies handed back first - this runs on the game half).
    if (ctx.renderer && ctx.gfx)
        m_weights.Release(ctx.gfx, &ctx.renderer->GetMeshManager().GetDescriptorHeap(), ctx.worldEpoch);
    else
        m_weights.Release(nullptr, nullptr, 0);
    if (!model.empty())
    {
        std::vector<BoneWeightHeat::Stream> ws;
        ws.reserve(blends.size());
        for (const auto& b : blends) ws.push_back({ b.meshIndex, b.weights.data(), b.weights.size() });
        std::string wwhy;
        if (!m_weights.Load(model, ws, wwhy)) LOG_WARNING("[SKELEDITOR] weights: %s", wwhy.c_str());
    }
    else
        m_weights.Clear();   // no picture: nothing to colour
    m_animRows.clear();
    m_animQueue.clear();
    m_animTotal = 0;
    m_animScanned.clear();
    m_animAutoScanned = false;
    m_path           = path;
    m_model          = model;
    m_iskel          = iskel;
    m_skel           = std::move(skel);
    m_report         = std::move(report);
    m_humanoidSource = humanoidSource;
    m_joints         = std::move(joints);
    m_axes           = std::move(axes);
    m_children       = std::move(children);
    m_names          = std::move(names);
    m_namesLower     = std::move(lower);
    m_boneIssues     = std::move(boneIssues);
    m_boneByName     = std::move(byName);
    // The streams stay for ReInspect; the profile as opened (the file's or the
    // runtime's lazy build) is what an unsaved humanoid edit is measured against.
    m_streams        = std::make_unique<WeightStreams>();
    m_streams->blends = std::move(blends);
    m_savedHumanoid  = m_skel->humanoid;
    m_modelLock      = model.empty() ? std::string() : FileLock(model, "model");
    m_iskelLock      = FileLock(iskel, "skeleton (.iskel)");
    m_iskelStamp     = FileStamp(iskel);
    m_iskelChangedSaid = false;
    m_sockets        = sockets;
    m_savedSockets   = std::move(sockets);
    m_vbs            = vbs;
    m_savedVbs       = std::move(vbs);
    m_undo.clear();
    m_redo.clear();
    m_selSocket.clear();
    m_selVb.clear();
    m_eulerFor = m_nameEditFor = m_vbNameEditFor = -1;
    m_nameError.clear();
    m_vbNameError.clear();
    m_pickTargetFrom = -1;
    m_staleInstances = 0;
    ReleaseGizmo();
    m_gizmoSocket = -1;
    for (int a = 0; a < 3; ++a) { m_boundsMin[a] = n ? lo[a] : 0.f; m_boundsMax[a] = n ? hi[a] : 0.f; }
    m_modelSize = std::max(1e-3f, m_report.stats.modelSize);
    m_loaded    = true;
    m_error.clear();
    m_selected  = -1;
    m_treeDirty = true;
    ResetView();
    RefreshVirtualBones();
    if (!m_model.empty() && ctx.staleVirtualBones) m_staleInstances = ctx.staleVirtualBones(m_model);
    m_attachLive.clear();
    LoadAttachments();   // this model's preview attachments (the user's list)
    m_poseEdits.clear();   // a pose is the subject's
    m_poseDirty = true;
    m_retargetMsg.clear();
    RefreshMask();   // the mask preview stays on, on this skeleton

    std::size_t skeletonSockets = 0, applied = 0;
    for (const Resource::ModelSocket& s : m_sockets) skeletonSockets += s.IsSkeletonSocket() ? 1u : 0u;
    for (const std::string& p : m_vbProblems) applied += p.empty() ? 1u : 0u;
    LOG_INFO("[SKELEDITOR] open: path='%s' iskel='%s' model='%s' bones=%u errors=%u warnings=%u infos=%u "
             "humanoid=%s joints=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) sockets=%zu virtualBones=%zu/%zu",
             m_path.c_str(), m_iskel.c_str(), m_model.empty() ? "(none)" : m_model.c_str(), n,
             m_report.Count(Resource::SkeletonIssueSeverity::Error),
             m_report.Count(Resource::SkeletonIssueSeverity::Warning),
             m_report.Count(Resource::SkeletonIssueSeverity::Info), m_humanoidSource.c_str(), m_boundsMin[0],
             m_boundsMin[1], m_boundsMin[2], m_boundsMax[0], m_boundsMax[1], m_boundsMax[2], skeletonSockets,
             applied, m_vbs.size());
    return true;
}

// ==== Selection / tree ====
bool SkeletonEditorWindow::Select(int bone, bool reveal)
{
    if (!m_loaded) return false;
    if (bone < -1 || bone >= static_cast<int>(m_names.size())) return false;
    m_selSocket.clear();   // a bone, not a socket (SelectSocket sets it after this)
    m_selVb.clear();       // nor a virtual bone (SelectVirtualBone)
    if (bone != m_selected)
    {
        m_revealSelected = reveal && bone >= 0;
        LOG_INFO("[SKELEDITOR] select -> %d '%s' (reveal=%d)", bone, bone >= 0 ? m_names[bone].c_str() : "",
                 reveal ? 1 : 0);
    }
    m_selected = bone;
    return true;
}

bool SkeletonEditorWindow::SelectByName(const std::string& name)
{
    for (std::size_t b = 0; b < m_names.size(); ++b)
        if (m_names[b] == name) return Select(static_cast<int>(b));
    return false;
}

void SkeletonEditorWindow::SetSearch(const std::string& text)
{
    std::snprintf(m_search, sizeof(m_search), "%s", text.c_str());
    m_treeDirty = true;
}

void SkeletonEditorWindow::SetOrbit(float yaw, float pitch, float distScale)
{
    m_yaw   = yaw;
    m_pitch = std::clamp(pitch, -1.4f, 1.4f);
    m_dist  = std::clamp(distScale, 0.05f, 8.f);
}

void SkeletonEditorWindow::ResetView()
{
    m_yaw = 0.f;
    m_pitch = 0.f;
    m_dist = 1.f;
    m_pan[0] = m_pan[1] = m_pan[2] = 0.f;
}

bool SkeletonEditorWindow::Matches(int b, const std::string& searchLower) const
{
    const auto& bi = m_report.bones[b];
    using namespace Resource;
    switch (m_filter)
    {
    case TreeFilter::All:      break;
    case TreeFilter::Weighted: if (!(bi.flags & kSkelBoneWeighted)) return false; break;
    case TreeFilter::Deform:   if (!(bi.flags & (kSkelBoneWeighted | kSkelBoneGrantTarget))) return false; break;
    case TreeFilter::IK:       if (!(bi.flags & (kSkelBoneIKBone | kSkelBoneIKTarget | kSkelBoneIKLink))) return false; break;
    case TreeFilter::Twist:    if (!(bi.flags & kSkelBoneTwist)) return false; break;
    case TreeFilter::Chain:    if (!(bi.flags & kSkelBoneChainName)) return false; break;
    case TreeFilter::Humanoid: if (!(bi.flags & kSkelBoneHumanoid)) return false; break;
    case TreeFilter::Issues:   if (m_boneIssues[b].empty()) return false; break;
    }
    return searchLower.empty() || m_namesLower[b].find(searchLower) != std::string::npos;
}

void SkeletonEditorWindow::RebuildRows()
{
    m_searchLower = LowerAscii(m_search);
    m_rows.clear();
    for (int b = 0; b < static_cast<int>(m_names.size()); ++b)
        if (Matches(b, m_searchLower)) m_rows.push_back(b);
    m_treeDirty = false;
}

void SkeletonEditorWindow::DrawBadges(int b) const
{
    using namespace Resource;
    const auto& bi = m_report.bones[b];
    auto badge = [](ImU32 col, const char* text, const char* tip)
    {
        ImGui::SameLine(0.f, 4.f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", text);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    if (bi.flags & kSkelBoneIKBone)      badge(kColIKBone, "IK", "an IK bone (the controller a chain solves toward)");
    if (bi.flags & kSkelBoneIKTarget)    badge(kColIKTarget, "IKt", "an IK chain's target (effector)");
    if (bi.flags & kSkelBoneIKLink)      badge(kColIKLink, "IKl", "a link an IK chain rotates");
    if (bi.flags & kSkelBoneGrantTarget) badge(kColGrant, "D", "copies another bone's rotation (付与)");
    if (bi.flags & kSkelBoneTwist)       badge(kColTwist, "Tw", "a twist bone (named 捩 / twist)");
    if (bi.flags & kSkelBoneChainName)   badge(kColChain, "Ch", "chain physics simulates it by name");
    if (bi.flags & kSkelBoneDummyLeaf)   badge(kColDummy, "o", "an unweighted leaf (PMX 錘 / 先, _end)");
    if (bi.humanoidRole >= 0)
        badge(IM_COL32(230, 230, 150, 255), HumanoidRoleName(static_cast<HumanoidBone>(bi.humanoidRole)),
              "its humanoid role (retargeting) - the Retarget tab / right-click > Humanoid Role changes it");
    if (InMask(b)) badge(kColMaskIn, "M", "in the bone mask (Retarget tab): a layer of that mask drives it");
    if (!m_boneIssues[b].empty())
    {
        auto worst = SkeletonIssueSeverity::Info;
        for (int i : m_boneIssues[b]) worst = std::max(worst, m_report.issues[i].severity);
        badge(worst == SkeletonIssueSeverity::Error ? IM_COL32(255, 80, 80, 255)
              : worst == SkeletonIssueSeverity::Warning ? IM_COL32(255, 180, 60, 255)
                                                                   : IM_COL32(150, 180, 255, 255),
              "!", m_report.issues[m_boneIssues[b].front()].detail.c_str());
    }
}

void SkeletonEditorWindow::DrawTreeNode(int b)
{
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                           ImGuiTreeNodeFlags_SpanAvailWidth;
    const std::vector<int>& socks = b < static_cast<int>(m_socketsByBone.size()) ? m_socketsByBone[b]
                                                                                 : m_socketsNoBone;   // never read
    const bool hasSockets = b < static_cast<int>(m_socketsByBone.size()) && !socks.empty();
    const bool hasVbs     = b < static_cast<int>(m_vbsByBone.size()) && !m_vbsByBone[b].empty();
    const bool leaf = m_children[b].empty() && !hasSockets && !hasVbs;
    if (leaf) f |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (b == m_selected && m_selSocket.empty() && m_selVb.empty()) f |= ImGuiTreeNodeFlags_Selected;
    // Reveal: open every ancestor of the selection once (and a selected socket's
    // or virtual bone's own bone - its row is a child row).
    if (m_revealSelected && m_selected >= 0 && !leaf)
    {
        if (b == m_selected && (!m_selSocket.empty() || !m_selVb.empty())) ImGui::SetNextItemOpen(true);
        for (int a = m_report.bones[m_selected].parent; a >= 0 && a < static_cast<int>(m_names.size());
             a = m_report.bones[a].parent)
            if (a == b) { ImGui::SetNextItemOpen(true); break; }
    }
    else if (m_report.bones[b].depth < 2)
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    char label[112];
    if (m_showIndex)
        std::snprintf(label, sizeof(label), "[%d] %s", b, m_names[b].empty() ? "(unnamed)" : m_names[b].c_str());
    else
        std::snprintf(label, sizeof(label), "%s", m_names[b].empty() ? "(unnamed)" : m_names[b].c_str());
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(b)), f, "%s", label);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Select(b, /*reveal=*/false);
    // A socket dropped here moves onto this bone (Ctrl: keeps its place).
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kSocketPayload))
        {
            const int si = *static_cast<const int*>(p->Data);
            if (si >= 0 && si < static_cast<int>(m_sockets.size()))
            {
                std::string why;
                const std::string name = m_sockets[si].name;
                if (!ReparentSocket(name, m_names[b], ImGui::GetIO().KeyCtrl, why)) m_error = why;
                else SelectSocket(name);
            }
        }
        ImGui::EndDragDropTarget();
    }
    // The bone's socket menu.
    ImGui::PushID(b);
    if (ImGui::BeginPopupContextItem("##ske_bone_ctx"))
    {
        const std::string lock = SocketLock();
        ImGui::BeginDisabled(!lock.empty());
        if (ImGui::MenuItem("Add Socket"))
        {
            std::string name, why;
            if (AddSocket(m_names[b], "", name, why)) SelectSocket(name);
            else m_error = why;
        }
        const char* clip = ImGui::GetClipboardText();
        const bool hasClip = clip && std::strncmp(clip, "SO ", 3) == 0;
        ImGui::BeginDisabled(!hasClip);
        std::vector<std::string> added;
        std::string why;
        if (ImGui::MenuItem("Paste Sockets") && !PasteSockets(clip ? clip : "", "", added, why)) m_error = why;
        if (ImGui::MenuItem("Paste Sockets To This Bone") && !PasteSockets(clip ? clip : "", m_names[b], added, why))
            m_error = why;
        ImGui::EndDisabled();
        if (!added.empty()) SelectSocket(added.back());
        // A virtual bone from this bone (UE's Add Virtual Bone > Pick Target Bone).
        ImGui::Separator();
        if (ImGui::BeginMenu("Add Virtual Bone"))
        {
            DrawVirtualBoneTargets(b);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Add Virtual Bone - Pick the Target in the Picture")) m_pickTargetFrom = b;
        ImGui::EndDisabled();
        if (!lock.empty()) ImGui::TextDisabled("%s", lock.c_str());
        // This bone's humanoid roles (the .iskel's - an .iskel opened alone too).
        ImGui::Separator();
        if (ImGui::BeginMenu("Humanoid Role"))
        {
            DrawHumanoidRoleMenu(b);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    if (b == m_selected && m_revealSelected && m_selSocket.empty() && m_selVb.empty())
    {
        ImGui::SetScrollHereY(0.35f);
        m_revealSelected = false;
    }
    DrawBadges(b);
    if (open && !leaf)
    {
        if (hasSockets)
            for (int si : socks) DrawSocketRow(si);
        if (hasVbs)
            for (int v : m_vbsByBone[b]) DrawVirtualBoneRow(v);
        for (int c : m_children[b]) DrawTreeNode(c);
        ImGui::TreePop();
    }
}

// One socket in the tree: select, drag onto a bone, a context menu.
void SkeletonEditorWindow::DrawSocketRow(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sockets.size())) return;
    const Resource::ModelSocket& s = m_sockets[index];
    const bool selected = !m_selSocket.empty() && s.IsSkeletonSocket() && s.name == m_selSocket;
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                           ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) f |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID(0x5000000 + index);
    ImGui::PushStyleColor(ImGuiCol_Text, kColSocketText);
    ImGui::TreeNodeEx("##sock", f, "<> %s%s", s.name.c_str(), s.orphan ? "  (orphan)" : "");
    ImGui::PopStyleColor();
    if (ImGui::IsItemClicked()) SelectSocket(s.name);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Socket on '%s'%s%s\nDrag onto a bone to move it there (Ctrl: keep its place).",
                          s.bone.c_str(), s.source.empty() ? "" : "  - from the source: ",
                          s.source.c_str());
    if (SocketLock().empty() && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload(kSocketPayload, &index, sizeof(index));
        ImGui::Text("Socket %s", s.name.c_str());
        ImGui::EndDragDropSource();
    }
    // A model / prefab dropped on a socket row: a preview attachment (UE's Add
    // Preview Asset).
    if (s.IsSkeletonSocket() && ImGui::BeginDragDropTarget())
    {
        std::string dropped;
        for (const char* type : { "ISCN_PATH", "IPFB_PATH" })
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) dropped = static_cast<const char*>(p->Data);
        ImGui::EndDragDropTarget();
        if (!dropped.empty())
        {
            std::string why;
            if (!AddAttachment(s.name, dropped, why)) m_error = why;
        }
    }
    if (selected && m_revealSelected)
    {
        ImGui::SetScrollHereY(0.35f);
        m_revealSelected = false;
    }
    if (ImGui::BeginPopupContextItem("##ske_sock_ctx"))
    {
        const std::string name = s.name;   // the list may change below
        ImGui::BeginDisabled(!SocketLock().empty());
        if (ImGui::MenuItem("Rename", "F2"))
        {
            m_renameFor = index;
            m_renameVb  = false;
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", name.c_str());
            m_renameOpen = true;
        }
        if (ImGui::MenuItem("Delete", "Del"))
        {
            std::string why;
            if (!RemoveSocket(name, why)) m_error = why;
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Copy", "Ctrl+C")) ImGui::SetClipboardText(CopySockets({ name }).c_str());
        if (ImGui::MenuItem("Copy All Sockets")) ImGui::SetClipboardText(CopySockets({}).c_str());
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void SkeletonEditorWindow::DrawTree()
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##ske_search", "Search bones...", m_search, sizeof(m_search))) m_treeDirty = true;
    int fi = 0;
    for (int i = 0; i < static_cast<int>(std::size(kFilters)); ++i)
        if (kFilters[i].f == m_filter) fi = i;
    ImGui::SetNextItemWidth(-60.f);
    if (ImGui::BeginCombo("##ske_filter", kFilters[fi].label))
    {
        for (int i = 0; i < static_cast<int>(std::size(kFilters)); ++i)
            if (ImGui::Selectable(kFilters[i].label, i == fi)) SetFilter(kFilters[i].f);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("#", &m_showIndex);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show bone indices");

    // The sockets by bone - a real bone, an applied virtual bone, or neither - and
    // the virtual bones by `from` (the staged lists may have changed since the last
    // draw).
    if (m_vbProblems.size() != m_vbs.size()) RefreshVirtualBones();   // never stale (every edit refreshes)
    m_socketsByBone.assign(m_names.size(), {});
    m_socketsByVb.assign(m_vbs.size(), {});
    m_socketsNoBone.clear();
    for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
    {
        if (!m_sockets[i].IsSkeletonSocket()) continue;
        const int b = BoneIndex(m_sockets[i].bone);
        const int v = b < 0 ? FindVirtualBone(m_sockets[i].bone) : -1;
        if (b >= 0)                                    m_socketsByBone[b].push_back(i);
        else if (v >= 0 && m_vbProblems[v].empty())    m_socketsByVb[v].push_back(i);
        else                                           m_socketsNoBone.push_back(i);
    }
    m_vbsByBone.assign(m_names.size(), {});
    m_vbsNoBone.clear();
    for (int v = 0; v < static_cast<int>(m_vbs.size()); ++v)
    {
        if (m_vbFrom[v] >= 0) m_vbsByBone[m_vbFrom[v]].push_back(v);
        else                  m_vbsNoBone.push_back(v);
    }

    ImGui::BeginChild("##ske_tree_rows", ImVec2(0.f, 0.f), false, ImGuiWindowFlags_HorizontalScrollbar);
    // Keys for the selected socket / virtual bone while the tree has focus (Ctrl+C /
    // V / Z come through EditorLayer's shortcut routing).
    if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() && !ImGui::GetIO().KeyCtrl && SocketLock().empty())
    {
        const int si = SelectedSocket();
        const int vi = si < 0 ? SelectedVirtualBone() : -1;
        if (si >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            std::string why;
            if (!RemoveSocket(m_sockets[si].name, why)) m_error = why;
        }
        else if (si >= 0 && ImGui::IsKeyPressed(ImGuiKey_F2, false))
        {
            m_renameFor = si;
            m_renameVb  = false;
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", m_sockets[si].name.c_str());
            m_renameOpen = true;
        }
        else if (vi >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            std::string why;
            if (!RemoveVirtualBone(m_vbs[vi].name, why)) m_error = why;
        }
        else if (vi >= 0 && ImGui::IsKeyPressed(ImGuiKey_F2, false))
        {
            m_renameFor = vi;
            m_renameVb  = true;
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", m_vbs[vi].name.c_str());
            m_renameOpen = true;
        }
    }
    const bool flat = m_filter != TreeFilter::All || m_search[0] != '\0';
    if (!flat)
    {
        if (!m_socketsNoBone.empty() &&
            ImGui::TreeNodeEx("##ske_nobone", ImGuiTreeNodeFlags_DefaultOpen, "Sockets without a bone (%zu)",
                              m_socketsNoBone.size()))
        {
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Their bone is not in this skeleton (a re-import renamed or removed it), or is a\n"
                                  "virtual bone that is not applied. Drag one onto a bone to give it one.");
            for (int si : m_socketsNoBone) DrawSocketRow(si);
            ImGui::TreePop();
        }
        if (!m_vbsNoBone.empty() &&
            ImGui::TreeNodeEx("##ske_vb_nobone", ImGuiTreeNodeFlags_DefaultOpen, "Virtual bones without a bone (%zu)",
                              m_vbsNoBone.size()))
        {
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Their `from` bone is not in this skeleton (a re-import renamed or removed it):\n"
                                  "kept in the file, never applied. Delete one, or re-import with its bone.");
            for (int v : m_vbsNoBone) DrawVirtualBoneRow(v);
            ImGui::TreePop();
        }
        for (int b = 0; b < static_cast<int>(m_names.size()); ++b)
            if (m_report.bones[b].parent < 0 || m_report.bones[b].parent >= static_cast<int>(m_names.size()))
                DrawTreeNode(b);
    }
    else
    {
        // Filtering flattens the hierarchy (UE's tree option): every match, in
        // bone order, its parent beside it.
        if (m_treeDirty) RebuildRows();
        if (m_rows.empty()) ImGui::TextDisabled("No bone matches.");
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()));
        while (clipper.Step())
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
            {
                const int b = m_rows[r];
                ImGui::PushID(b);
                char label[112];
                if (m_showIndex) std::snprintf(label, sizeof(label), "[%d] %s", b, m_names[b].c_str());
                else             std::snprintf(label, sizeof(label), "%s", m_names[b].c_str());
                if (ImGui::Selectable(label, b == m_selected)) Select(b, /*reveal=*/false);
                if (b == m_selected && m_revealSelected)
                {
                    ImGui::SetScrollHereY(0.35f);
                    m_revealSelected = false;
                }
                DrawBadges(b);
                const int p = m_report.bones[b].parent;
                if (p >= 0 && p < static_cast<int>(m_names.size()))
                {
                    ImGui::SameLine(0.f, 8.f);
                    ImGui::TextDisabled("< %s", m_names[p].c_str());
                }
                ImGui::PopID();
            }
        // A search finds sockets and virtual bones too (by name, the filter "all" only).
        if (m_filter == TreeFilter::All && m_search[0] != '\0')
        {
            for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
                if (m_sockets[i].IsSkeletonSocket() && LowerAscii(m_sockets[i].name).find(m_searchLower) != std::string::npos)
                    DrawSocketRow(i);
            for (int v = 0; v < static_cast<int>(m_vbs.size()); ++v)
                if (LowerAscii(m_vbs[v].name).find(m_searchLower) != std::string::npos) DrawVirtualBoneRow(v);
        }
    }
    ImGui::EndChild();
}

void SkeletonEditorWindow::DrawDetails(const SkeletonEditorContext& ctx)
{
    using namespace Resource;
    if (SelectedSocket() >= 0)
    {
        DrawSocketDetails(ctx);
        return;
    }
    if (SelectedVirtualBone() >= 0)
    {
        DrawVirtualBoneDetails(ctx);
        return;
    }
    if (m_selected < 0 || m_selected >= static_cast<int>(m_names.size()))
    {
        ImGui::TextDisabled("Select a bone - in the tree, or click near a joint in the picture.");
        return;
    }
    const int b = m_selected;
    const SkeletonBoneInfo& bi = m_report.bones[b];
    ImGui::Text("[%d] %s", b, m_names[b].c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(m_names[b].c_str());
    // Its sockets.
    {
        std::vector<int> socks;
        for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
            if (m_sockets[i].IsSkeletonSocket() && m_sockets[i].bone == m_names[b]) socks.push_back(i);
        ImGui::TextDisabled("Sockets");
        for (int si : socks)
        {
            ImGui::SameLine();
            ImGui::PushID(si);
            ImGui::PushStyleColor(ImGuiCol_Text, kColSocketText);
            if (ImGui::SmallButton(m_sockets[si].name.c_str())) SelectSocket(m_sockets[si].name);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::SameLine();
        const std::string lock = SocketLock();
        ImGui::BeginDisabled(!lock.empty());
        if (ImGui::SmallButton("+ Add Socket"))
        {
            std::string name, why;
            if (AddSocket(m_names[b], "", name, why)) SelectSocket(name);
            else m_error = why;
        }
        ImGui::EndDisabled();
        if (!lock.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", lock.c_str());
    }
    // Its virtual bones (from it), and the ones that follow it (to it).
    {
        ImGui::TextDisabled("Virtual bones");
        for (int v = 0; v < static_cast<int>(m_vbs.size()); ++v)
        {
            if (VirtualBoneFrom(v) != b) continue;
            ImGui::SameLine();
            ImGui::PushID(0x7100000 + v);
            ImGui::PushStyleColor(ImGuiCol_Text, m_vbProblems[v].empty() ? kColVirtualText : kColWarnText);
            if (ImGui::SmallButton(m_vbs[v].name.c_str())) SelectVirtualBone(m_vbs[v].name);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::SameLine();
        const std::string lock = SocketLock();
        ImGui::BeginDisabled(!lock.empty());
        if (ImGui::SmallButton("+ Add Virtual Bone")) ImGui::OpenPopup(kVbPickPopup);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", !lock.empty() ? lock.c_str()
                                                  : "A bone that is where the target bone is every frame, as a child of\n"
                                                    "this one (UE's virtual bone): an IK target, a socket's bone.");
        if (ImGui::BeginPopup(kVbPickPopup))
        {
            DrawVirtualBoneTargets(b);
            ImGui::EndPopup();
        }
        std::vector<int> following;
        for (int v = 0; v < static_cast<int>(m_vbs.size()); ++v)
            if (VirtualBoneTo(v) == b && m_vbProblems[v].empty()) following.push_back(v);
        if (!following.empty())
        {
            ImGui::TextDisabled("Followed by");
            for (int v : following)
            {
                ImGui::SameLine();
                ImGui::PushID(0x7200000 + v);
                ImGui::PushStyleColor(ImGuiCol_Text, kColVirtualText);
                if (ImGui::SmallButton(m_vbs[v].name.c_str())) SelectVirtualBone(m_vbs[v].name);
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
    }

    auto boneLink = [&](const char* label, int bone)
    {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine();
        if (bone < 0 || bone >= static_cast<int>(m_names.size())) { ImGui::TextDisabled("-"); return; }
        ImGui::PushID(label);
        if (ImGui::SmallButton(m_names[bone].c_str())) Select(bone);
        ImGui::PopID();
    };
    boneLink("Parent  ", bi.parent);
    ImGui::TextDisabled("Depth %u, %u child(ren), length %.4f", bi.depth, bi.childCount, bi.length);
    if (!m_children[b].empty() && ImGui::TreeNode("##ske_children", "Children (%zu)", m_children[b].size()))
    {
        for (int c : m_children[b])
        {
            ImGui::PushID(c);
            if (ImGui::SmallButton(m_names[c].c_str())) Select(c);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    std::string flags;
    for (const char* f : SkeletonBoneFlagNames(bi.flags)) flags += (flags.empty() ? "" : ", ") + std::string(f);
    ImGui::TextWrapped("Categories: %s", flags.empty() ? "-" : flags.c_str());
    ImGui::Separator();

    // Rest local TRS (the reference pose, parent-relative) and the bind position.
    XMVECTOR s, r, t;
    if (XMMatrixDecompose(&s, &r, &t, XMLoadFloat4x4(&m_skel->restPoseLocal[b])))
    {
        XMFLOAT3 tt, ss;
        XMFLOAT4 q;
        XMStoreFloat3(&tt, t);
        XMStoreFloat3(&ss, s);
        XMStoreFloat4(&q, r);
        const XMFLOAT3 e = MathUtils::RollPitchYawDegreesFromQuaternion(q);
        ImGui::Text("Rest local");
        ImGui::TextDisabled("  T %.4f  %.4f  %.4f", tt.x, tt.y, tt.z);
        ImGui::TextDisabled("  R %.2f  %.2f  %.2f  (deg)", e.x, e.y, e.z);
        ImGui::TextDisabled("  S %.4f  %.4f  %.4f", ss.x, ss.y, ss.z);
    }
    else
        ImGui::TextDisabled("Rest local: not decomposable");
    ImGui::Text("Bind (model)");
    ImGui::TextDisabled("  %.4f  %.4f  %.4f", m_joints[b * 3], m_joints[b * 3 + 1], m_joints[b * 3 + 2]);
    // The Animation host: the pose now (the clip, grants, virtual bones; the
    // serial window's copy, one frame old).
    if (StageShown() && m_live.selValid && m_live.selBone == b)
    {
        const XMFLOAT3 e = MathUtils::RollPitchYawDegreesFromQuaternion(m_live.selRot);
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.f, 1.f), "Live local (%s)",
                           m_clipPlayer.Path().empty() ? "rest pose" : "animated");
        ImGui::TextDisabled("  T %.4f  %.4f  %.4f", m_live.selPos.x, m_live.selPos.y, m_live.selPos.z);
        ImGui::TextDisabled("  R %.2f  %.2f  %.2f  (deg)", e.x, e.y, e.z);
        ImGui::TextDisabled("  S %.4f  %.4f  %.4f", m_live.selScl.x, m_live.selScl.y, m_live.selScl.z);
        if (m_live.valid && static_cast<std::size_t>(b) * 3 + 2 < m_live.joints.size())
            ImGui::TextDisabled("  world %.4f  %.4f  %.4f", m_live.joints[b * 3], m_live.joints[b * 3 + 1],
                                m_live.joints[b * 3 + 2]);
    }
    // Temporary posing on the stage: the bone turned in its own frame on top of
    // whatever poses it (to see the skin deform); nothing is written anywhere.
    if (StageShown())
    {
        XMFLOAT3 deg = { 0.f, 0.f, 0.f };
        for (const auto& [bone, d] : m_poseEdits)
            if (bone == b) deg = d;
        float v[3] = { deg.x, deg.y, deg.z };
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.f, 1.f), "Pose (temporary)");
        ImGui::SetNextItemWidth(-90.f);
        if (ImGui::DragFloat3("##ske_pose", v, 0.5f, -180.f, 180.f, "%.1f deg"))
            SetBonePose(b, XMFLOAT3(v[0], v[1], v[2]));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Turn it in its own frame (the Inspector's degrees) on top of the clip - to see the\n"
                              "skin deform. Kept across clips; never saved.");
        ImGui::SameLine();
        if (ImGui::SmallButton("0##ske_pose0")) SetBonePose(b, XMFLOAT3(0.f, 0.f, 0.f));
        ImGui::SameLine();
        ImGui::BeginDisabled(m_poseEdits.empty());
        if (ImGui::SmallButton("Reset Pose")) ResetPose();
        ImGui::EndDisabled();
    }
    ImGui::Separator();

    if (m_report.stats.weightsKnown)
        ImGui::Text("Skin: %u vertices, max weight %.3f", bi.weightedVerts, bi.maxWeight);
    else
        ImGui::TextDisabled("Skin: weights unknown (the .iskel carries no blend streams)");
    if (bi.grantSource >= 0)
    {
        char l[64];
        std::snprintf(l, sizeof(l), "付与 x%.3f from", bi.grantRatio);
        boneLink(l, bi.grantSource);
    }
    if (bi.flags & kSkelBoneGrantSource)
    {
        ImGui::TextDisabled("付与 source of:");
        for (int o = 0; o < static_cast<int>(m_names.size()); ++o)
            if (m_report.bones[o].grantSource == b)
            {
                ImGui::SameLine();
                ImGui::PushID(o);
                if (ImGui::SmallButton(m_names[o].c_str())) Select(o);
                ImGui::PopID();
            }
    }
    for (std::size_t c = 0; c < m_skel->ikChains.size(); ++c)
    {
        const auto& ch = m_skel->ikChains[c];
        const char* role = nullptr;
        if (ch.ikBoneIndex == static_cast<std::uint32_t>(b)) role = "the IK bone";
        else if (ch.targetBoneIndex == static_cast<std::uint32_t>(b)) role = "the target";
        else
            for (const auto& l : ch.links)
                if (l.boneIndex == static_cast<std::uint32_t>(b)) role = "a link";
        if (!role) continue;
        const std::uint32_t ik = ch.ikBoneIndex;
        ImGui::TextDisabled("IK chain %zu (%s): %s", c, ik < m_names.size() ? m_names[ik].c_str() : "?", role);
    }
    // Every role on it (a profile may give one bone several), staged.
    {
        std::string roles;
        const HumanoidProfile& hp = m_skel->humanoid;
        for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r)
            if (hp.boneForRole[r] == b)
                roles += (roles.empty() ? "" : ", ") + std::string(HumanoidRoleName(static_cast<HumanoidBone>(r)));
        if (hp.hipsTranslationBone == b) roles += roles.empty() ? "hipsTranslation" : ", hipsTranslation";
        if (!roles.empty()) ImGui::Text("Humanoid: %s", roles.c_str());
    }
    if (!m_boneIssues[b].empty())
    {
        ImGui::Separator();
        for (int i : m_boneIssues[b])
        {
            const SkeletonIssue& is = m_report.issues[i];
            ImGui::TextColored(is.severity == SkeletonIssueSeverity::Error     ? ImVec4(1.f, 0.4f, 0.4f, 1.f)
                               : is.severity == SkeletonIssueSeverity::Warning ? ImVec4(1.f, 0.72f, 0.3f, 1.f)
                                                                               : ImVec4(0.6f, 0.75f, 1.f, 1.f),
                               "%s", SkeletonIssueKindName(is.kind));
            ImGui::TextWrapped("%s", is.detail.c_str());
        }
    }
}

void SkeletonEditorWindow::DrawCheck()
{
    using namespace Resource;
    // The virtual bones': one not applied (ApplyVirtualBones' reason; an orphan is
    // info), one that follows a post-IK grant (MMD append) target (it misses that
    // rotation).
    {
        struct Row { int vb; std::string text; bool warn; };
        std::vector<Row> rows;
        for (int v = 0; v < static_cast<int>(m_vbs.size()); ++v)
        {
            if (!m_vbProblems[v].empty())
                rows.push_back({ v, "not applied: " + m_vbProblems[v], !m_vbs[v].orphan });
            else if (PostIKGrant(m_vbTo[v]))
                rows.push_back({ v, "'" + m_vbs[v].to + "' copies a rotation AFTER IK (付与): the virtual bone, posed "
                                    "before IK, does not have it", false });
        }
        if (!rows.empty())
        {
            ImGui::TextDisabled("Virtual bones");
            for (const Row& r : rows)
            {
                ImGui::PushID(0x6100000 + r.vb);
                ImGui::TextColored(r.warn ? kColWarnText : ImVec4(0.6f, 0.75f, 1.f, 1.f), "%-7s",
                                   r.warn ? "warning" : "info");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, kColVirtualText);
                if (ImGui::SmallButton(m_vbs[r.vb].name.c_str())) SelectVirtualBone(m_vbs[r.vb].name);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", r.text.c_str());
                ImGui::PopID();
            }
            ImGui::Separator();
        }
    }
    // The sockets' problems: a bone the skeleton lacks, an orphan, two names that
    // differ only by case (Engine.EquipToSocket is case-sensitive).
    {
        struct Row { int socket; std::string text; bool warn; };
        std::vector<Row> rows;
        for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
        {
            const ModelSocket& s = m_sockets[i];
            if (!s.IsSkeletonSocket()) continue;
            if (BindBoneOf(s.bone) < 0)
                rows.push_back({ i, s.orphan ? "an orphan: its bone '" + s.bone + "' went with a re-import"
                                             : "names bone '" + s.bone + "', which this skeleton does not have (nor an "
                                               "applied virtual bone) - a spawn does not publish it", !s.orphan });
            for (int j = 0; j < i; ++j)
                if (m_sockets[j].IsSkeletonSocket() && m_sockets[j].name != s.name &&
                    LowerAscii(m_sockets[j].name) == LowerAscii(s.name))
                    rows.push_back({ i, "differs from '" + m_sockets[j].name + "' only by case - socket lookups are "
                                        "case-sensitive", true });
        }
        if (!rows.empty())
        {
            ImGui::TextDisabled("Sockets");
            for (const Row& r : rows)
            {
                ImGui::PushID(0x6000000 + r.socket);
                ImGui::TextColored(r.warn ? ImVec4(1.f, 0.72f, 0.3f, 1.f) : ImVec4(0.6f, 0.75f, 1.f, 1.f), "%-7s",
                                   r.warn ? "warning" : "info");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, kColSocketText);
                if (ImGui::SmallButton(m_sockets[r.socket].name.c_str())) SelectSocket(m_sockets[r.socket].name);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", r.text.c_str());
                ImGui::PopID();
            }
            ImGui::Separator();
        }
    }
    const auto& issues = m_report.issues;
    if (issues.empty())
    {
        ImGui::TextDisabled("No problems found in the skeleton.");
        return;
    }
    ImGui::TextDisabled("%u error(s), %u warning(s), %u info", m_report.Count(SkeletonIssueSeverity::Error),
                        m_report.Count(SkeletonIssueSeverity::Warning), m_report.Count(SkeletonIssueSeverity::Info));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(issues.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const SkeletonIssue& is = issues[i];
            ImGui::PushID(i);
            const ImVec4 col = is.severity == SkeletonIssueSeverity::Error     ? ImVec4(1.f, 0.4f, 0.4f, 1.f)
                             : is.severity == SkeletonIssueSeverity::Warning ? ImVec4(1.f, 0.72f, 0.3f, 1.f)
                                                                             : ImVec4(0.6f, 0.75f, 1.f, 1.f);
            ImGui::TextColored(col, "%-7s", SkeletonIssueSeverityName(is.severity));
            ImGui::SameLine();
            if (is.bone >= 0 && is.bone < static_cast<int>(m_names.size()))
            {
                if (ImGui::SmallButton(m_names[is.bone].c_str())) Select(is.bone);
                ImGui::SameLine();
            }
            ImGui::TextDisabled("%s", SkeletonIssueKindName(is.kind));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", is.detail.c_str());
            ImGui::PopID();
        }
}

void SkeletonEditorWindow::DrawStatsLine() const
{
    const Resource::SkeletonStats& s = m_report.stats;
    ImGui::TextDisabled("%u bones, depth %u  |  IK %u  |  付与 %u  |  twist %u  |  weighted %u / %u  |  "
                        "humanoid %s%s  |  E%u W%u I%u", s.bones, s.maxDepth, s.ikChains, s.grantTargets, s.twist,
                        s.weighted, s.bones, m_humanoidSource.c_str(), s.humanoidValid ? " (valid)" : "",
                        m_report.Count(Resource::SkeletonIssueSeverity::Error),
                        m_report.Count(Resource::SkeletonIssueSeverity::Warning),
                        m_report.Count(Resource::SkeletonIssueSeverity::Info));
}

// ==== Sockets: the model's SO records, staged ====
namespace
{
    // A byte count that does not cut a UTF-8 sequence (bone names are Japanese).
    std::string Utf8Prefix(const std::string& s, std::size_t maxBytes)
    {
        if (s.size() <= maxBytes) return s;
        std::size_t n = maxBytes;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
        return s.substr(0, n);
    }

    bool Finite(const Resource::ModelSocket& s)
    {
        const float v[] = { s.t.x, s.t.y, s.t.z, s.q.x, s.q.y, s.q.z, s.q.w, s.s.x, s.s.y, s.s.z };
        for (float f : v)
            if (!std::isfinite(f)) return false;
        return true;
    }

    bool SameQuat(const XMFLOAT4& a, const XMFLOAT4& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
}

const char* SkeletonEditorWindow::GizmoOpName(GizmoOp op)
{
    switch (op)
    {
    case GizmoOp::Off:       return "off";
    case GizmoOp::Translate: return "translate";
    case GizmoOp::Rotate:    return "rotate";
    case GizmoOp::Scale:     return "scale";
    }
    return "?";
}

bool SkeletonEditorWindow::ParseGizmoOp(const std::string& s, GizmoOp& out)
{
    const std::string l = LowerAscii(s);
    for (GizmoOp op : { GizmoOp::Off, GizmoOp::Translate, GizmoOp::Rotate, GizmoOp::Scale })
        if (l == GizmoOpName(op)) { out = op; return true; }
    return false;
}

int SkeletonEditorWindow::BoneIndex(const std::string& name) const
{
    const auto it = std::lower_bound(m_boneByName.begin(), m_boneByName.end(), name,
                                     [](const std::pair<std::string, int>& e, const std::string& n) { return e.first < n; });
    return (it != m_boneByName.end() && it->first == name) ? it->second : -1;
}

std::string SkeletonEditorWindow::SocketLock() const
{
    if (!m_loaded) return "nothing is open";
    if (m_model.empty())
        return "no model names this skeleton here - sockets live in the model file (open the model, not the .iskel)";
    if (!m_modelLock.empty()) return m_modelLock + " - its sockets and virtual bones";
    return {};
}

int SkeletonEditorWindow::FindSocket(const std::string& name) const
{
    for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
        if (m_sockets[i].IsSkeletonSocket() && m_sockets[i].name == name) return i;
    return -1;
}

bool SkeletonEditorWindow::SelectSocket(const std::string& name)
{
    if (name.empty()) { m_selSocket.clear(); return true; }
    const int i = FindSocket(name);
    if (i < 0) return false;
    // Its row's bone: a real bone, or the `from` of the virtual bone it sits on.
    int b = BoneIndex(m_sockets[i].bone);
    if (b < 0)
        if (const int v = FindVirtualBone(m_sockets[i].bone); v >= 0 && m_vbProblems[v].empty()) b = m_vbFrom[v];
    if (b >= 0) Select(b, /*reveal=*/true);   // clears the socket selection: set it after
    m_selSocket      = name;
    m_revealSelected = true;
    LOG_INFO("[SKELEDITOR] select socket -> '%s' (bone '%s')", name.c_str(), m_sockets[i].bone.c_str());
    return true;
}

void SkeletonEditorWindow::PushUndo()
{
    m_undo.push_back({ m_sockets, m_vbs, Humanoid() });
    if (m_undo.size() > kUndoDepth) m_undo.pop_front();
    m_redo.clear();
}

bool SkeletonEditorWindow::StageSockets(std::vector<Resource::ModelSocket> next, const char* what, std::string& why,
                                        bool pushUndo)
{
    return StageRecords(std::move(next), m_vbs, what, why, pushUndo);
}

bool SkeletonEditorWindow::StageRecords(std::vector<Resource::ModelSocket> next,
                                        std::vector<Resource::ModelVirtualBone> vbs, const char* what,
                                        std::string& why, bool pushUndo)
{
    if (const std::string lock = SocketLock(); !lock.empty()) { why = lock; return false; }
    for (std::size_t i = 0; i < next.size(); ++i)
    {
        if (const std::string p = Resource::SocketNameProblem(next[i].name); !p.empty())
        {
            why = p + " ('" + next[i].name + "')";
            return false;
        }
        if (!Finite(next[i])) { why = "socket '" + next[i].name + "' has a value that is not a number"; return false; }
        for (std::size_t j = 0; j < i; ++j)
            if (next[j].meshKey == next[i].meshKey && next[j].name == next[i].name)
            {
                why = "two sockets are named '" + next[i].name + "'";
                return false;
            }
    }
    // The virtual bones' names: legal and unique (a file's repeat keeps the last
    // record, so a staged list never holds two). Their rules are the editing verbs'.
    for (std::size_t i = 0; i < vbs.size(); ++i)
    {
        if (const std::string p = Resource::VirtualBoneNameProblem(vbs[i].name); !p.empty())
        {
            why = p + " ('" + vbs[i].name + "')";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j)
            if (vbs[j].name == vbs[i].name) { why = "two virtual bones are named '" + vbs[i].name + "'"; return false; }
    }
    const bool vbChanged = vbs != m_vbs;
    if (next == m_sockets && !vbChanged) return true;   // no change, no undo step
    if (pushUndo) PushUndo();
    CarryAttachments(m_sockets, next);   // a renamed socket's preview attachments follow it
    m_sockets = std::move(next);
    if (vbChanged)
    {
        m_vbs = std::move(vbs);
        RefreshVirtualBones();
    }
    LOG_INFO("[SKELEDITOR] %s: %s - %zu socket(s), %zu virtual bone(s) staged, dirty=%d, undo=%zu",
             vbChanged ? "virtual bones" : "sockets", what, m_sockets.size(), m_vbs.size(), Dirty() ? 1 : 0,
             m_undo.size());
    return true;
}

std::string SkeletonEditorWindow::UniqueSocketName(const std::string& base) const
{
    auto used = [&](const std::string& n) { return FindSocket(n) >= 0; };
    const std::string b = Utf8Prefix(base, 63);
    if (!used(b)) return b;
    for (int k = 1; k < 10000; ++k)
    {
        const std::string suffix = "_" + std::to_string(k);
        const std::string n = Utf8Prefix(base, 63 - suffix.size()) + suffix;
        if (!used(n)) return n;
    }
    return b;
}

std::vector<int> SkeletonEditorWindow::SocketsOfBone(int bone) const
{
    std::vector<int> out;
    if (bone < 0 || bone >= static_cast<int>(m_names.size())) return out;
    for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
        if (m_sockets[i].IsSkeletonSocket() && m_sockets[i].bone == m_names[bone]) out.push_back(i);
    return out;
}

bool SkeletonEditorWindow::AddSocket(const std::string& bone, const std::string& name, std::string& outName,
                                     std::string& why)
{
    if (const std::string lock = SocketLock(); !lock.empty()) { why = lock; return false; }
    if (BindBoneOf(bone) < 0) { why = "no bone '" + bone + "' in this skeleton (nor an applied virtual bone)"; return false; }
    if (!name.empty() && FindSocket(name) >= 0) { why = "a socket named '" + name + "' exists"; return false; }
    Resource::ModelSocket s;
    s.name = name.empty() ? UniqueSocketName(Utf8Prefix(bone, 56) + "_socket") : name;
    s.bone = bone;
    auto next = m_sockets;
    next.push_back(s);
    if (!StageSockets(std::move(next), ("add '" + s.name + "' on '" + bone + "'").c_str(), why)) return false;
    outName = s.name;
    return true;
}

bool SkeletonEditorWindow::SetSocket(const std::string& name, const Resource::ModelSocket& value, std::string& why)
{
    const int i = FindSocket(name);
    if (i < 0) { why = "no socket '" + name + "'"; return false; }
    Resource::ModelSocket v = value;
    v.meshKey = 0;
    if (v.bone != m_sockets[i].bone && BindBoneOf(v.bone) < 0) { why = "no bone '" + v.bone + "' in this skeleton"; return false; }
    if (v.name != name && FindSocket(v.name) >= 0) { why = "a socket named '" + v.name + "' exists"; return false; }
    const float ql = std::sqrt(v.q.x * v.q.x + v.q.y * v.q.y + v.q.z * v.q.z + v.q.w * v.q.w);
    if (!(ql > 1e-6f)) { why = "the rotation has zero length"; return false; }
    v.q = { v.q.x / ql, v.q.y / ql, v.q.z / ql, v.q.w / ql };
    if (v.s.x == 0.f || v.s.y == 0.f || v.s.z == 0.f) { why = "a scale of 0 collapses the socket"; return false; }
    // An edited socket is the author's (a re-import keeps it); its bone decides orphan.
    if (!(v == m_sockets[i])) v.source.clear();
    v.orphan = BindBoneOf(v.bone) < 0;
    auto next = m_sockets;
    next[static_cast<std::size_t>(i)] = v;
    if (!StageSockets(std::move(next), ("set '" + name + "'").c_str(), why)) return false;
    if (m_selSocket == name) m_selSocket = v.name;
    // A rename from anywhere (F2, Lua): the Details' Name field shows the new name
    // (its buffer is keyed by the row, which did not change).
    if (v.name != name) m_nameEditFor = -1;
    return true;
}

bool SkeletonEditorWindow::RemoveSocket(const std::string& name, std::string& why)
{
    const int i = FindSocket(name);
    if (i < 0) { why = "no socket '" + name + "'"; return false; }
    auto next = m_sockets;
    next.erase(next.begin() + i);
    if (!StageSockets(std::move(next), ("remove '" + name + "'").c_str(), why)) return false;
    if (m_selSocket == name) m_selSocket.clear();
    return true;
}

bool SkeletonEditorWindow::ReparentSocket(const std::string& name, const std::string& bone, bool keepWorld,
                                          std::string& why)
{
    const int i = FindSocket(name);
    if (i < 0) { why = "no socket '" + name + "'"; return false; }
    const int b = BindBoneOf(bone);   // a virtual bone's bind place is its `to`'s
    if (b < 0) { why = "no bone '" + bone + "' in this skeleton (nor an applied virtual bone)"; return false; }
    Resource::ModelSocket v = m_sockets[i];
    if (v.bone == bone) return true;
    XMFLOAT4X4 model;
    if (keepWorld && SocketModelMatrix(i, model))
    {
        // Its place in the bind pose, under the new bone: model x inverse(bind(new)).
        XMFLOAT4X4 off;
        XMStoreFloat4x4(&off, XMMatrixMultiply(XMLoadFloat4x4(&model), XMLoadFloat4x4(&m_skel->inverseBindPose[b])));
        if (!v.SetFromOffset(off)) { why = "the socket's place does not decompose under '" + bone + "'"; return false; }
    }
    v.bone   = bone;
    v.orphan = false;
    v.source.clear();
    auto next = m_sockets;
    next[static_cast<std::size_t>(i)] = v;
    return StageSockets(std::move(next),
                        ("reparent '" + name + "' onto '" + bone + (keepWorld ? "' (kept its place)" : "'")).c_str(),
                        why);
}

bool SkeletonEditorWindow::EditSocket(const std::string& name, const std::string* bone, bool keepWorld,
                                      const std::function<void(Resource::ModelSocket&)>& edit, std::string& why)
{
    if (FindSocket(name) < 0) { why = "no socket '" + name + "'"; return false; }
    // All or nothing: what is staged, and the history, put back on a refusal.
    const std::vector<Resource::ModelSocket> sockets = m_sockets;
    const auto        undo = m_undo;
    const auto        redo = m_redo;
    const std::string sel  = m_selSocket;
    auto restore = [&]
    {
        m_sockets   = sockets;
        m_undo      = undo;
        m_redo      = redo;
        m_selSocket = sel;
    };
    const std::size_t depth = m_undo.size();
    if (bone && !ReparentSocket(name, *bone, keepWorld, why)) { restore(); return false; }
    const int i = FindSocket(name);
    if (i < 0) { restore(); why = "no socket '" + name + "'"; return false; }
    Resource::ModelSocket v = m_sockets[static_cast<std::size_t>(i)];
    edit(v);
    if (!(v == m_sockets[static_cast<std::size_t>(i)]) && !SetSocket(name, v, why)) { restore(); return false; }
    // A reparent and a set: one step - the first snapshot (before both) stays.
    if (m_undo.size() == depth + 2) m_undo.pop_back();
    return true;
}

std::string SkeletonEditorWindow::CopySockets(const std::vector<std::string>& names) const
{
    std::vector<Resource::ModelSocket> out;
    for (const Resource::ModelSocket& s : m_sockets)
        if (s.IsSkeletonSocket() && (names.empty() || std::find(names.begin(), names.end(), s.name) != names.end()))
            out.push_back(s);
    return Resource::FormatSockets(out);
}

bool SkeletonEditorWindow::PasteSockets(const std::string& text, const std::string& toBone,
                                        std::vector<std::string>& added, std::string& why)
{
    added.clear();
    if (const std::string lock = SocketLock(); !lock.empty()) { why = lock; return false; }
    if (!toBone.empty() && BindBoneOf(toBone) < 0) { why = "no bone '" + toBone + "' in this skeleton"; return false; }
    Resource::ModelSockets parsed;
    Resource::ParseModelSockets(text, parsed);
    auto next = m_sockets;
    std::size_t skipped = 0;
    for (Resource::ModelSocket s : parsed.sockets)
    {
        if (!s.IsSkeletonSocket()) continue;
        if (!toBone.empty()) s.bone = toBone;
        if (BindBoneOf(s.bone) < 0) { ++skipped; continue; }
        s.source.clear();
        s.orphan = false;
        // A name in use (the staged list, or one pasted just now) gets a new one.
        auto inUse = [&](const std::string& n)
        {
            return std::any_of(next.begin(), next.end(),
                               [&](const Resource::ModelSocket& o) { return o.IsSkeletonSocket() && o.name == n; });
        };
        if (inUse(s.name))
            for (int k = 1; k < 10000; ++k)
            {
                const std::string suffix = "_" + std::to_string(k);
                const std::string n = Utf8Prefix(s.name, 63 - suffix.size()) + suffix;
                if (!inUse(n)) { s.name = n; break; }
            }
        next.push_back(s);
        added.push_back(s.name);
    }
    if (added.empty())
    {
        why = parsed.sockets.empty() ? "no socket records (SO lines) in the text"
                                     : "none of the pasted sockets' bones is in this skeleton";
        return false;
    }
    if (!StageSockets(std::move(next), ("paste " + std::to_string(added.size()) + " socket(s)" +
                                        (toBone.empty() ? std::string() : " onto '" + toBone + "'")).c_str(), why))
    {
        added.clear();
        return false;
    }
    if (skipped) LOG_INFO("[SKELEDITOR] paste: %zu socket(s) skipped - their bone is not in this skeleton", skipped);
    return true;
}

bool SkeletonEditorWindow::Undo()
{
    if (m_undo.empty() || m_gizmoUsing) return false;
    m_redo.push_back({ m_sockets, m_vbs, Humanoid() });
    CarryAttachments(m_sockets, m_undo.back().sockets);
    m_sockets = std::move(m_undo.back().sockets);
    m_vbs     = std::move(m_undo.back().vbs);
    const HumanoidProfile hp = m_undo.back().humanoid;
    m_undo.pop_back();
    RefreshVirtualBones();
    RestoreHumanoid(hp);
    m_eulerFor = m_nameEditFor = m_vbNameEditFor = -1;
    LOG_INFO("[SKELEDITOR] undo: dirty=%d undo=%zu redo=%zu", Dirty() ? 1 : 0, m_undo.size(), m_redo.size());
    return true;
}

bool SkeletonEditorWindow::Redo()
{
    if (m_redo.empty() || m_gizmoUsing) return false;
    m_undo.push_back({ m_sockets, m_vbs, Humanoid() });
    CarryAttachments(m_sockets, m_redo.back().sockets);
    m_sockets = std::move(m_redo.back().sockets);
    m_vbs     = std::move(m_redo.back().vbs);
    const HumanoidProfile hp = m_redo.back().humanoid;
    m_redo.pop_back();
    RefreshVirtualBones();
    RestoreHumanoid(hp);
    m_eulerFor = m_nameEditFor = m_vbNameEditFor = -1;
    LOG_INFO("[SKELEDITOR] redo: dirty=%d undo=%zu redo=%zu", Dirty() ? 1 : 0, m_undo.size(), m_redo.size());
    return true;
}

bool SkeletonEditorWindow::Revert()
{
    if (!m_loaded) return false;
    const bool records  = SocketsDirty() || VirtualBonesDirty();
    const bool humanoid = HumanoidDirty();
    if (!records && !humanoid) return true;   // nothing to revert, no undo step
    PushUndo();
    std::string why;
    if (records && !StageRecords(m_savedSockets, m_savedVbs, "revert to the file", why, /*pushUndo=*/false))
    {
        m_undo.pop_back();
        return false;
    }
    if (humanoid) RestoreHumanoid(m_savedHumanoid);
    LOG_INFO("[SKELEDITOR] revert: records=%d humanoid=%d undo=%zu", records ? 1 : 0, humanoid ? 1 : 0, m_undo.size());
    return true;
}

bool SkeletonEditorWindow::SaveNow(const SkeletonEditorContext& ctx, std::string& why, SaveReport* rep,
                                   bool forceHumanoid)
{
    if (!m_loaded) { why = "nothing is open"; return false; }
    if (ctx.playing) { why = "the editor is playing - Stop first (a saved model re-derives its sockets into the world)"; return false; }
    if (m_gizmoUsing) { why = "a gizmo drag is in progress"; return false; }
    // The humanoid profile goes into the .iskel; an .iskel opened alone saves only
    // that.
    const bool humanoid = HumanoidDirty() || forceHumanoid;
    const std::string lock = SocketLock();
    if (!lock.empty() && !humanoid) { why = lock + " - and the humanoid mapping has no unsaved edit"; return false; }
    // The humanoid write is refused before anything is written: a read-only
    // .iskel, or one that changed on disk since it was opened (a re-import may have
    // reordered its bones; the Retarget window may have saved its own mapping).
    if (humanoid && !m_iskelLock.empty()) { why = m_iskelLock + " - its humanoid mapping"; return false; }
    if (humanoid && FileStamp(m_iskel) != m_iskelStamp)
    {
        why = "the skeleton file '" + m_iskel + "' changed on disk since it was opened - reopen it and map again: "
              "this mapping's bone indices are the old file's";
        return false;
    }
    SaveReport r;
    r.model = m_model;
    if (lock.empty())
    {
        // The staged lists passed StageRecords; a direct Details edit did not rename.
        // The VB and SO records go in one write.
        const std::uint64_t vbSigBefore = Resource::VirtualBonesSignature(m_savedVbs);
        std::string before, after, err;
        if (!SceneInstanceLoader::ReplaceModelSkeletonRecords(m_model, m_sockets, m_vbs, before, after, &err))
        {
            why = err.empty() ? "the model file was not written" : err;
            LOG_WARNING("[SKELEDITOR] save '%s' failed: %s", m_model.c_str(), why.c_str());
            return false;
        }
        m_savedSockets = m_sockets;
        m_savedVbs     = m_vbs;
        r.written      = before != after;
        r.records      = static_cast<std::uint32_t>(m_sockets.size());
        r.virtualBones = static_cast<std::uint32_t>(m_vbs.size());
        r.refreshed    = ctx.socketsChanged ? ctx.socketsChanged(m_model) : 0u;
        // A placed instance's skeleton takes new virtual bones only when it spawns again.
        m_staleInstances = ctx.staleVirtualBones ? ctx.staleVirtualBones(m_model) : 0u;
        r.stale          = m_staleInstances;
        // So does the stage: the panel spawns its own again.
        if (m_stage.Up() && Resource::VirtualBonesSignature(m_vbs) != vbSigBefore) m_stageRespawn = true;
    }
    if (humanoid)
    {
        // The Animation Retarget window's write (Editor/HumanoidMapping): the section at the
        // payload's end, patched or appended. The World's skeletons of this .iskel take it
        // now (a placement's retarget / layer masks; the stage's).
        std::string err;
        if (!SaveHumanoidProfileToIskel(m_iskel, m_skel->humanoid, &err))
        {
            why = err;
            LOG_WARNING("[SKELEDITOR] save humanoid '%s' failed: %s", m_iskel.c_str(), why.c_str());
            if (rep) *rep = r;
            return false;
        }
        m_savedHumanoid    = m_skel->humanoid;
        m_iskelStamp       = FileStamp(m_iskel);   // our own write
        m_humanoidSource   = "file";
        r.humanoidWritten  = true;
        r.humanoidPushed   = ctx.humanoidChanged ? ctx.humanoidChanged(m_iskel, m_skel->humanoid, *m_skel) : 0u;
    }
    char line[256];
    if (lock.empty())
        std::snprintf(line, sizeof(line), "Saved %u socket record(s) and %u virtual bone(s)%s; %u placed instance(s) "
                      "follow%s", r.records, r.virtualBones, r.written ? "" : " (the file already said so)",
                      r.refreshed, r.humanoidWritten ? "; the humanoid mapping too" : "");
    else
        std::snprintf(line, sizeof(line), "Saved the humanoid mapping into the .iskel; %u skeleton(s) in the world "
                      "follow", r.humanoidPushed);
    m_lastSave = line;
    LOG_INFO("[SKELEDITOR] save: model='%s' written=%d records=%u virtualBones=%u refreshed=%u stale=%u "
             "humanoid=%d pushed=%u iskel='%s'", m_model.c_str(), r.written ? 1 : 0, r.records, r.virtualBones,
             r.refreshed, r.stale, r.humanoidWritten ? 1 : 0, r.humanoidPushed, m_iskel.c_str());
    if (rep) *rep = r;
    return true;
}

// ==== The humanoid mapping and the bone mask ====
const HumanoidProfile& SkeletonEditorWindow::Humanoid() const
{
    static const HumanoidProfile kNone;
    return m_skel ? m_skel->humanoid : kNone;
}

bool SkeletonEditorWindow::HumanoidDirty() const
{
    // Byte for byte: the profile is what the .iskel stores verbatim (no padding).
    return m_loaded && m_skel && std::memcmp(&m_skel->humanoid, &m_savedHumanoid, sizeof(HumanoidProfile)) != 0;
}

void SkeletonEditorWindow::ReInspect()
{
    if (!m_skel) return;
    std::vector<Resource::SkinWeightStream> streams;
    if (m_streams)
        for (const auto& b : m_streams->blends) streams.push_back({ b.weights.data(), b.weights.size() });
    m_report = Resource::InspectSkeleton(*m_skel, streams);
    std::vector<std::vector<int>> boneIssues(m_names.size());
    for (std::size_t i = 0; i < m_report.issues.size(); ++i)
        if (m_report.issues[i].bone >= 0 && m_report.issues[i].bone < static_cast<int>(m_names.size()))
            boneIssues[m_report.issues[i].bone].push_back(static_cast<int>(i));
    m_boneIssues = std::move(boneIssues);
    m_treeDirty  = true;
}

namespace
{
    std::uint32_t MappedRoles(const HumanoidProfile& hp)
    {
        std::uint32_t n = 0;
        for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r) n += hp.boneForRole[r] >= 0 ? 1u : 0u;
        return n;
    }
}

bool SkeletonEditorWindow::EditHumanoid(const char* what, const std::function<void(SkeletonAsset&)>& edit)
{
    if (!m_loaded || !m_skel) return false;
    const HumanoidProfile before = m_skel->humanoid;
    edit(*m_skel);
    if (std::memcmp(&before, &m_skel->humanoid, sizeof(HumanoidProfile)) == 0) return false;   // no change, no step
    m_undo.push_back({ m_sockets, m_vbs, before });
    if (m_undo.size() > kUndoDepth) m_undo.pop_front();
    m_redo.clear();
    ReInspect();
    RefreshMask();
    const HumanoidProfile& hp = m_skel->humanoid;
    LOG_INFO("[SKELEDITOR] humanoid: %s - mapped=%u valid=%u userEdited=%u dirty=%d undo=%zu", what, MappedRoles(hp),
             hp.valid, hp.userEdited, HumanoidDirty() ? 1 : 0, m_undo.size());
    return true;
}

void SkeletonEditorWindow::RestoreHumanoid(const HumanoidProfile& hp)
{
    if (!m_skel || std::memcmp(&hp, &m_skel->humanoid, sizeof(HumanoidProfile)) == 0) return;
    m_skel->humanoid = hp;
    ReInspect();
    RefreshMask();
}

bool SkeletonEditorWindow::SetHumanoidRole(HumanoidBone role, int bone, std::string& why)
{
    if (!m_loaded || !m_skel) { why = "nothing is open"; return false; }
    if (!m_iskelLock.empty()) { why = m_iskelLock + " - its humanoid mapping"; return false; }
    const std::uint32_t r = static_cast<std::uint32_t>(role);
    if (r >= kHumanoidBoneCount) { why = "no such humanoid role"; return false; }
    if (bone < -1 || bone >= static_cast<int>(m_skel->RealBoneCount())) { why = "no such bone"; return false; }
    // A hand edit, as the Retarget window's rows make one.
    EditHumanoid(("role " + std::string(HumanoidRoleName(role)) + " -> " +
                  (bone < 0 ? std::string("(none)") : m_names[bone])).c_str(),
                 [&](SkeletonAsset& sk)
                 {
                     if (sk.humanoid.boneForRole[r] == bone) return;
                     sk.humanoid.boneForRole[r] = bone;
                     sk.humanoid.userEdited     = 1;
                     Resource::HumanoidProfileBuilder::FinalizeDerived(sk);
                 });
    return true;
}

bool SkeletonEditorWindow::SetHipsTranslation(int bone, std::string& why)
{
    if (!m_loaded || !m_skel) { why = "nothing is open"; return false; }
    if (!m_iskelLock.empty()) { why = m_iskelLock + " - its humanoid mapping"; return false; }
    if (bone < -1 || bone >= static_cast<int>(m_skel->RealBoneCount())) { why = "no such bone"; return false; }
    EditHumanoid("hips translation", [&](SkeletonAsset& sk)
    {
        if (sk.humanoid.hipsTranslationBone == bone) return;
        sk.humanoid.hipsTranslationBone = bone;
        sk.humanoid.userEdited          = 1;
        Resource::HumanoidProfileBuilder::FinalizeDerived(sk);
    });
    return true;
}

bool SkeletonEditorWindow::AutoDetectHumanoid()
{
    if (!m_iskelLock.empty()) return false;   // read-only: a packed / junction .iskel
    // The Retarget window's Auto-Detect: the builder's map, marked not hand-edited.
    return EditHumanoid("auto-detect", [](SkeletonAsset& sk)
    {
        sk.humanoid.userEdited = 0;
        Resource::HumanoidProfileBuilder::Build(sk);
    });
}

bool SkeletonEditorWindow::ClearHumanoid()
{
    if (!m_iskelLock.empty()) return false;   // read-only: a packed / junction .iskel
    return EditHumanoid("clear all", [](SkeletonAsset& sk)
    {
        sk.humanoid.Reset();
        sk.humanoid.userEdited = 1;
        Resource::HumanoidProfileBuilder::FinalizeDerived(sk);
    });
}

bool SkeletonEditorWindow::SelectRole(HumanoidBone role)
{
    const std::uint32_t r = static_cast<std::uint32_t>(role);
    if (!m_skel || r >= kHumanoidBoneCount) return false;
    const int b = m_skel->humanoid.boneForRole[r];
    return b >= 0 && Select(b, /*reveal=*/true);
}

void SkeletonEditorWindow::RefreshMask()
{
    m_maskW.clear();
    m_maskResolved = true;
    if (m_maskMode < 0 || !m_skel) return;
    // The runtime's own function (an AnimLayer's mask) on the skeleton the runtime
    // builds it on: the staged profile, and the staged virtual bones after the real
    // ones - a Custom Root may name one, and a subtree holds them.
    if (m_vbs.empty())
    {
        m_maskResolved = AnimationSystem::BuildBoneMask(*m_skel, static_cast<std::uint8_t>(m_maskMode), m_maskRoot, m_maskW);
        return;
    }
    auto withVbs = std::make_unique<SkeletonAsset>(*m_skel);   // ~650 KB: on the heap
    Resource::ApplyVirtualBones(*withVbs, m_vbs, nullptr);
    m_maskResolved = AnimationSystem::BuildBoneMask(*withVbs, static_cast<std::uint8_t>(m_maskMode), m_maskRoot, m_maskW);
}

void SkeletonEditorWindow::SetMask(int mode, const std::string& root)
{
    m_maskMode = (mode >= 0 && mode <= static_cast<int>(BoneMaskMode::CustomRoot)) ? mode : -1;
    m_maskRoot = root;
    RefreshMask();
    LOG_INFO("[SKELEDITOR] mask: mode=%s root='%s' resolved=%d bones=%u", MaskModeName(m_maskMode), m_maskRoot.c_str(),
             m_maskResolved ? 1 : 0, MaskBones());
}

std::uint32_t SkeletonEditorWindow::MaskBones() const
{
    std::uint32_t n = 0;
    if (m_maskMode >= 0)
        for (float w : m_maskW) n += w > 0.5f ? 1u : 0u;
    return n;
}

bool SkeletonEditorWindow::InMask(int bone) const
{
    return m_maskMode >= 0 && bone >= 0 && bone < static_cast<int>(m_maskW.size()) && m_maskW[bone] > 0.5f;
}

void SkeletonEditorWindow::RequestMaskPreview(const std::string& model, int mode, const std::string& root)
{
    const std::string rel = Sys::RelativizeAssetPath(model);
    m_open = true;
    RequestTab(Tab::Retarget);
    if (m_loaded && (LowerAscii(rel) == LowerAscii(m_model) || LowerAscii(rel) == LowerAscii(m_path)))
    {
        SetMask(mode, root);
        return;
    }
    RequestOpen(rel);
    m_maskQueued      = true;
    m_maskQueuedMode  = mode;
    m_maskQueuedRoot  = root;
    m_maskQueuedModel = rel;
}

// The Retarget tab: the humanoid profile (the Animation Retarget window's rows), the
// way to that window, the bone mask preview.
void SkeletonEditorWindow::DrawRetarget(const SkeletonEditorContext& ctx)
{
    if (!m_skel) return;
    const HumanoidProfile& hp = m_skel->humanoid;
    std::string missing;
    for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r)
        if (IsRequiredHumanoidBone(static_cast<HumanoidBone>(r)) && hp.boneForRole[r] < 0)
            missing += (missing.empty() ? "" : ", ") + std::string(HumanoidRoleName(static_cast<HumanoidBone>(r)));
    ImGui::TextColored(hp.IsValid() ? ImVec4(0.55f, 0.9f, 0.55f, 1.f) : ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s",
                       hp.IsValid() ? "Humanoid: valid" : "Humanoid: INVALID");
    ImGui::SameLine();
    ImGui::TextDisabled("%u / %u roles%s", MappedRoles(hp), kHumanoidBoneCount, HumanoidDirty() ? " | unsaved" : "");
    ImGui::TextDisabled("%s names | from the %s%s", HumanoidConventionName(hp.convention),
                        m_humanoidSource == "file" ? ".iskel" : m_humanoidSource == "built" ? "auto-detect" : "(none)",
                        hp.userEdited ? " | hand-edited" : "");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("From the .iskel: its humanoid section maps these roles. From the auto-detect: the file maps\n"
                          "none, so the runtime builds this (by bone name) when it needs one - what retargeting and\n"
                          "the Upper / Lower Body layer masks read.");
    if (!missing.empty())
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "Required and unmapped: %s", missing.c_str());
    const bool readOnly = !m_iskelLock.empty();   // a packed / junction .iskel
    if (readOnly) ImGui::TextDisabled("%s", m_iskelLock.c_str());

    ImGui::BeginDisabled(readOnly);
    if (ImGui::Button("Auto-Detect")) AutoDetectHumanoid();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Map every role by bone name (HumanoidProfileBuilder) - the hand edits go.");
    ImGui::SameLine();
    if (ImGui::Button("Clear All")) ClearHumanoid();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(ctx.playing || readOnly);
    if (ImGui::Button("Save to .iskel"))
    {
        // Draw is the render-side half: the write and the World's skeletons wait for PPA.
        m_forceHumanoidSave = true;
        m_saveQueued        = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes the mapping into '%s' (its humanoid section) - with the model's socket and\n"
                          "virtual bone records when they have unsaved edits. The skeletons in the world\n"
                          "made from this .iskel take it at once.", m_iskel.c_str());
    ImGui::BeginDisabled(m_model.empty() || ctx.playing);
    if (ImGui::Button("Open Animation Retarget with this target")) m_retargetQueued = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Selects a placed instance of this model (the retarget bakes onto a scene entity)\n"
                          "and opens the Animation Retarget window on it.");
    if (!m_retargetMsg.empty()) ImGui::TextColored(kColWarnText, "%s", m_retargetMsg.c_str());

    ImGui::Separator();
    ImGui::BeginChild("##ske_roles", ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing() * 5.5f), false);
    ImGui::BeginDisabled(readOnly);
    EditHumanoid("mapping rows", [&](SkeletonAsset& sk)
    {
        DrawHumanoidRoleRows(sk, m_roleFilter, sizeof(m_roleFilter),
                             [this](HumanoidBone, int bone) { if (bone >= 0) Select(bone, /*reveal=*/true); });
    });
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::Separator();
    DrawMaskPreview();
}

// The bone mask preview: the bones an AnimLayer of a mask drives.
void SkeletonEditorWindow::DrawMaskPreview()
{
    ImGui::TextUnformatted("Bone mask (an Anim Layer's)");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The bones an Anim Layer with this mask drives - AnimationSystem::BuildBoneMask, the\n"
                          "runtime's own, on this skeleton and its (staged) humanoid profile. Green in the picture,\n"
                          "M in the tree. Upper Body = the humanoid spine's subtree; Lower Body = the rest.");
    int idx = 0;
    for (int i = 0; i < static_cast<int>(std::size(kMasks)); ++i)
        if (kMasks[i].mode == m_maskMode) idx = i;
    ImGui::SetNextItemWidth(230.f);
    if (ImGui::BeginCombo("##ske_mask", kMasks[idx].label))
    {
        for (int i = 0; i < static_cast<int>(std::size(kMasks)); ++i)
            if (ImGui::Selectable(kMasks[i].label, i == idx)) SetMask(kMasks[i].mode, m_maskRoot);
        ImGui::EndCombo();
    }
    if (m_maskMode == static_cast<int>(BoneMaskMode::CustomRoot) && m_skel)
    {
        std::int32_t root = -1;
        for (std::uint32_t b = 0; b < m_skel->boneCount && root < 0; ++b)
            if (m_maskRoot == m_names[b]) root = static_cast<std::int32_t>(b);
        const std::int32_t was = root;
        if (HumanoidBonePickerRow("Root bone", false, *m_skel, root, m_roleFilter, sizeof(m_roleFilter)) && root != was)
            SetMask(m_maskMode, root >= 0 ? m_names[root] : std::string());
        ImGui::BeginDisabled(m_selected < 0);
        if (ImGui::SmallButton("Use the selected bone") && m_selected >= 0) SetMask(m_maskMode, m_names[m_selected]);
        ImGui::EndDisabled();
    }
    if (m_maskMode >= 0)
    {
        if (m_maskResolved)
            ImGui::TextDisabled("%u of %u bones in the mask%s", MaskBones(), MaskSize(),
                                MaskSize() > m_names.size() ? " (its virtual bones counted)" : "");
        else
            ImGui::TextColored(kColWarnText, "Does not resolve (%s): the layer would play FULL BODY",
                               m_maskMode == static_cast<int>(BoneMaskMode::CustomRoot) ? "no bone of that name"
                                                                                        : "no humanoid spine");
    }
}

// The tree's context menu: map this bone to a role (a role maps ONE bone - it moves
// here), or take a role off it.
void SkeletonEditorWindow::DrawHumanoidRoleMenu(int bone)
{
    if (!m_skel || bone < 0 || bone >= static_cast<int>(m_skel->RealBoneCount())) return;
    const HumanoidProfile& hp = m_skel->humanoid;
    std::size_t n = 0;
    const HumanoidRoleGroup* groups = HumanoidRoleGroups(n);
    std::string why;
    for (std::size_t g = 0; g < n; ++g)
    {
        if (!ImGui::BeginMenu(groups[g].name)) continue;
        for (std::size_t i = 0; i < groups[g].count; ++i)
        {
            const HumanoidBone  role = groups[g].roles[i];
            const std::int32_t  cur  = hp.boneForRole[static_cast<std::uint32_t>(role)];
            const char* on = cur >= 0 && cur < static_cast<std::int32_t>(m_names.size()) ? m_names[cur].c_str() : "";
            if (ImGui::MenuItem(HumanoidRoleName(role), on, cur == bone))
                SetHumanoidRole(role, cur == bone ? -1 : bone, why);
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Root motion (hipsTranslation)", nullptr, hp.hipsTranslationBone == bone))
        SetHipsTranslation(hp.hipsTranslationBone == bone ? -1 : bone, why);
    bool any = false;
    for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r) any |= hp.boneForRole[r] == bone;
    ImGui::BeginDisabled(!any);
    if (ImGui::MenuItem("Clear this bone's roles"))
        EditHumanoid("clear a bone's roles", [bone](SkeletonAsset& sk)
        {
            for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r)
                if (sk.humanoid.boneForRole[r] == bone) sk.humanoid.boneForRole[r] = -1;
            sk.humanoid.userEdited = 1;
            Resource::HumanoidProfileBuilder::FinalizeDerived(sk);
        });
    ImGui::EndDisabled();
}

bool SkeletonEditorWindow::SocketModelMatrix(int index, XMFLOAT4X4& out) const
{
    if (!m_skel || index < 0 || index >= static_cast<int>(m_sockets.size())) return false;
    const Resource::ModelSocket& s = m_sockets[index];
    if (!s.IsSkeletonSocket()) return false;
    const int b = BindBoneOf(s.bone);   // on a virtual bone, its `to`'s bind place
    if (b < 0) return false;
    XMVECTOR det;
    const XMMATRIX bind = XMMatrixInverse(&det, XMLoadFloat4x4(&m_skel->inverseBindPose[b]));
    const XMFLOAT4X4 off = s.LocalOffset();
    XMStoreFloat4x4(&out, XMMatrixMultiply(XMLoadFloat4x4(&off), bind));
    return true;
}

bool SkeletonEditorWindow::SocketDisplayMatrix(int index, const float centre[3], float scale, XMFLOAT4X4& out) const
{
    XMFLOAT4X4 m;
    if (!SocketModelMatrix(index, m)) return false;
    const XMMATRIX fit = XMMatrixMultiply(XMMatrixTranslation(-centre[0], -centre[1], -centre[2]),
                                          XMMatrixScaling(scale, scale, scale));
    XMStoreFloat4x4(&out, XMMatrixMultiply(XMLoadFloat4x4(&m), fit));
    return true;
}

bool SkeletonEditorWindow::ApplyDisplayMatrix(int index, const XMFLOAT4X4& display, const float centre[3],
                                              float scale, GizmoOp op)
{
    if (!m_skel || index < 0 || index >= static_cast<int>(m_sockets.size()) || !(scale > 0.f)) return false;
    const Resource::ModelSocket cur = m_sockets[index];
    const int b = BindBoneOf(cur.bone);
    if (b < 0) return false;
    const XMMATRIX fitInv = XMMatrixMultiply(XMMatrixScaling(1.f / scale, 1.f / scale, 1.f / scale),
                                             XMMatrixTranslation(centre[0], centre[1], centre[2]));
    const XMMATRIX model = XMMatrixMultiply(XMLoadFloat4x4(&display), fitInv);
    XMFLOAT4X4 off;
    XMStoreFloat4x4(&off, XMMatrixMultiply(model, XMLoadFloat4x4(&m_skel->inverseBindPose[b])));
    Resource::ModelSocket n = cur;
    if (!n.SetFromOffset(off)) return false;
    // Only what the operation moves: the rest stays bit for bit (no drift).
    switch (op)
    {
    case GizmoOp::Translate: n.q = cur.q; n.s = cur.s; break;
    case GizmoOp::Rotate:    n.t = cur.t; n.s = cur.s; break;
    case GizmoOp::Scale:     n.t = cur.t; n.q = cur.q; break;
    case GizmoOp::Off:       return false;
    }
    if (!Finite(n) || n == cur) return false;
    n.source.clear();
    m_sockets[static_cast<std::size_t>(index)] = n;
    return true;
}

// The stage host's gizmo result: the socket's new place in the world, on the
// live pose: offset = world x inverse(the bone's live world matrix). Only what
// the operation moves changes (the rest stays bit for bit).
bool SkeletonEditorWindow::ApplyLiveMatrix(int index, const XMFLOAT4X4& world, GizmoOp op)
{
    if (!m_live.valid || index < 0 || index >= static_cast<int>(m_sockets.size())) return false;
    const Resource::ModelSocket cur = m_sockets[index];
    const int b = BindBoneOf(cur.bone);
    if (b < 0 || b >= static_cast<int>(m_live.boneWorld.size())) return false;
    XMVECTOR det;
    const XMMATRIX inv = XMMatrixInverse(&det, XMLoadFloat4x4(&m_live.boneWorld[b]));
    XMFLOAT4X4 off;
    XMStoreFloat4x4(&off, XMMatrixMultiply(XMLoadFloat4x4(&world), inv));
    Resource::ModelSocket n = cur;
    if (!n.SetFromOffset(off)) return false;
    switch (op)
    {
    case GizmoOp::Translate: n.q = cur.q; n.s = cur.s; break;
    case GizmoOp::Rotate:    n.t = cur.t; n.s = cur.s; break;
    case GizmoOp::Scale:     n.t = cur.t; n.q = cur.q; break;
    case GizmoOp::Off:       return false;
    }
    if (!Finite(n) || n == cur) return false;
    n.source.clear();
    m_sockets[static_cast<std::size_t>(index)] = n;
    return true;
}

void SkeletonEditorWindow::SetGizmoOp(GizmoOp op)
{
    if (m_gizmoUsing) return;   // never mid-drag (it strands ImGuizmo's mbUsing)
    m_gizmoOp = op;
}

bool SkeletonEditorWindow::GizmoDrag(const std::string& name, GizmoOp op, const XMFLOAT3& v, float deg,
                                     std::string& why)
{
    if (const std::string lock = SocketLock(); !lock.empty()) { why = lock; return false; }
    const int i = FindSocket(name);
    if (i < 0) { why = "no socket '" + name + "'"; return false; }
    if (op == GizmoOp::Off) { why = "no operation"; return false; }
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(deg))
    {
        why = "a value is not a number";
        return false;
    }
    // Any fit round-trips exactly - it is what the interactive gizmo's matrix goes through.
    const float centre[3] = { 0.5f * (m_boundsMin[0] + m_boundsMax[0]), 0.5f * (m_boundsMin[1] + m_boundsMax[1]),
                              0.5f * (m_boundsMin[2] + m_boundsMax[2]) };
    const float scale = 1.f / std::max(1e-3f, m_modelSize);
    XMFLOAT4X4 g;
    if (!SocketDisplayMatrix(i, centre, scale, g)) { why = "its bone '" + m_sockets[i].bone + "' is not in this skeleton"; return false; }
    XMMATRIX G = XMLoadFloat4x4(&g);
    switch (op)
    {
    case GizmoOp::Translate:   // the model's axes, in its units
        G = XMMatrixMultiply(G, XMMatrixTranslation(v.x * scale, v.y * scale, v.z * scale));
        break;
    case GizmoOp::Rotate:      // about the socket's LOCAL axis (ImGuizmo's local rotation)
    {
        const XMVECTOR axis = XMVector3Normalize(XMVectorSet(v.x, v.y, v.z, 0.f));
        if (XMVector3Equal(axis, XMVectorZero()) || XMVector3IsNaN(axis)) { why = "the axis is zero"; return false; }
        G = XMMatrixMultiply(XMMatrixRotationAxis(axis, XMConvertToRadians(deg)), G);
        break;
    }
    case GizmoOp::Scale:
        if (!(v.x > 0.f && v.y > 0.f && v.z > 0.f)) { why = "scale factors must be positive"; return false; }
        G = XMMatrixMultiply(XMMatrixScaling(v.x, v.y, v.z), G);
        break;
    case GizmoOp::Off:
        break;
    }
    XMStoreFloat4x4(&g, G);
    Staged before{ m_sockets, m_vbs, Humanoid() };   // the whole staged state (an undo restores all of it)
    if (!ApplyDisplayMatrix(i, g, centre, scale, op)) { why = "the result is the same socket or not a valid one"; return false; }
    m_undo.push_back(std::move(before));
    if (m_undo.size() > kUndoDepth) m_undo.pop_front();
    m_redo.clear();
    ++m_gizmoGestures;
    LOG_INFO("[SKELEDITOR] socket gizmo: '%s' op=%s v=(%.4f,%.4f,%.4f) deg=%.2f gestures=%u", name.c_str(),
             GizmoOpName(op), v.x, v.y, v.z, deg, m_gizmoGestures);
    return true;
}

// Every path that ends our drag from this side (the gizmo vanished mid-drag, the
// panel stopped drawing) hands ImGuizmo's global mbUsing / editing ID back.
void SkeletonEditorWindow::ReleaseGizmo()
{
    if (!m_gizmoUsing) return;
    ImGuizmo::Enable(false);
    ImGuizmo::Enable(true);
    m_gizmoUsing  = false;
    m_gizmoPushed = false;
}

void SkeletonEditorWindow::DrawSocketGizmo(Renderer* renderer, float x, float y, float w, float h)
{
    const bool wasOver = m_gizmoOver;
    m_gizmoOver = false;
    bool submitted = false;
    const int i = SelectedSocket();
    float centre[3] = { 0.f, 0.f, 0.f }, scale = 1.f;
    XMFLOAT4X4 display, viewF, projF;
    // On the stage host the gizmo works in the world, on the live pose, with the
    // camera the panel gave the stage; on the reference host in the studio's unit
    // box.
    const bool stage = StageShown();
    bool have = false;
    if (stage)
    {
        have  = m_live.valid && SocketFrameMatrix(i, display);
        viewF = m_live.view;
        projF = m_live.proj;
    }
    else if (renderer && w > 1.f && h > 1.f && renderer->GetPreviewAssetFit(m_model, centre, scale) &&
             SocketDisplayMatrix(i, centre, scale, display))
    {
        have = true;
        const PreviewPanel::Cam cam = PreviewPanel::BuildCam(m_yaw, m_pitch, m_dist, m_pan, w / h);
        XMStoreFloat4x4(&viewF, cam.view);
        XMStoreFloat4x4(&projF, cam.proj);
    }
    const bool want = m_gizmoOp != GizmoOp::Off && SocketLock().empty() && (m_show & kShowSockets) != 0 && i >= 0 &&
                      renderer && w > 1.f && h > 1.f && have;
    if (want)
    {
        // Freeze the fed matrix while hovered / held: a round trip through the
        // staged socket would drift the drag.
        if (m_gizmoSocket != i || (!m_gizmoUsing && !wasOver && !ImGuizmo::IsUsingAny()))
        {
            m_gizmoSocket = i;
            m_gizmoMatrix = display;
        }
        float matrix[16];
        std::memcpy(matrix, &m_gizmoMatrix, sizeof(matrix));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(x, y, w, h);
        ImGuizmo::SetDrawlist(dl);
        ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());
        ImGuizmo::PushID(kSocketGizmoId);
        const ImGuizmo::OPERATION gop = m_gizmoOp == GizmoOp::Translate ? ImGuizmo::TRANSLATE
                                      : m_gizmoOp == GizmoOp::Rotate    ? ImGuizmo::ROTATE
                                                                        : ImGuizmo::SCALE;
        const bool manipulated = ImGuizmo::Manipulate(reinterpret_cast<const float*>(&viewF),
                                                      reinterpret_cast<const float*>(&projF), gop,
                                                      m_gizmoOp == GizmoOp::Translate ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
                                                      matrix);
        const bool usingNow = ImGuizmo::IsUsing();
        m_gizmoOver = ImGuizmo::IsOver() || usingNow;
        ImGuizmo::PopID();
        dl->PopClipRect();
        submitted = true;
        if (manipulated && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            XMFLOAT4X4 result;
            std::memcpy(&result, matrix, sizeof(result));
            // One drag, one undo step: the staged lists before its first change.
            std::optional<Staged> before;
            if (!m_gizmoPushed) before = Staged{ m_sockets, m_vbs, Humanoid() };
            if (stage ? ApplyLiveMatrix(i, result, m_gizmoOp) : ApplyDisplayMatrix(i, result, centre, scale, m_gizmoOp))
            {
                if (before)
                {
                    m_undo.push_back(std::move(*before));
                    if (m_undo.size() > kUndoDepth) m_undo.pop_front();
                    m_redo.clear();
                    m_gizmoPushed = true;
                    ++m_gizmoGestures;
                }
                m_gizmoMatrix = result;
            }
        }
        m_gizmoUsing = usingNow;
        if (!usingNow) m_gizmoPushed = false;
    }
    if (!submitted && m_gizmoUsing) ReleaseGizmo();   // a drag whose gizmo vanished
}

int SkeletonEditorWindow::PickSocketAt(Renderer* renderer, float px, float py) const
{
    const std::uint32_t view = CurrentView();   // the host drawn now
    if (!m_loaded || view == ~0u || !renderer || !(m_show & kShowSockets)) return -1;
    std::uint32_t ow = 0, oh = 0;
    if (!renderer->GetPreviewOutputSize(view, ow, oh) || !ow || !oh) return -1;
    XMFLOAT4X4 toClip, toFrame;
    const float* joints = nullptr;
    const float* axes   = nullptr;
    if (!FrameFor(renderer, static_cast<float>(ow), static_cast<float>(oh), toClip, toFrame, joints, axes)) return -1;
    const Projection pr = ProjectionOf(toClip, 0.f, 0.f, static_cast<float>(ow), static_cast<float>(oh));
    int best = -1;
    float bestD2 = kPickRadiusPx * kPickRadiusPx;
    for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
    {
        XMFLOAT4X4 m;
        if (!SocketFrameMatrix(i, m)) continue;
        const float p[3] = { m._41, m._42, m._43 };
        ImVec2 q;
        if (!pr.Project(p, q)) continue;
        const float dx = q.x - px, dy = q.y - py, d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    return best;
}

bool SkeletonEditorWindow::ProjectSocket(Renderer* renderer, int index, float& px, float& py) const
{
    const std::uint32_t view = CurrentView();
    if (!m_loaded || view == ~0u || !renderer) return false;
    std::uint32_t ow = 0, oh = 0;
    if (!renderer->GetPreviewOutputSize(view, ow, oh) || !ow || !oh) return false;
    XMFLOAT4X4 toClip, toFrame, m;
    const float* joints = nullptr;
    const float* axes   = nullptr;
    if (!SocketFrameMatrix(index, m) ||
        !FrameFor(renderer, static_cast<float>(ow), static_cast<float>(oh), toClip, toFrame, joints, axes))
        return false;
    const Projection pr = ProjectionOf(toClip, 0.f, 0.f, static_cast<float>(ow), static_cast<float>(oh));
    const float p[3] = { m._41, m._42, m._43 };
    ImVec2 q;
    if (!pr.Project(p, q)) return false;
    px = q.x;
    py = q.y;
    return true;
}

void SkeletonEditorWindow::DrawSocketDetails(const SkeletonEditorContext& ctx)
{
    const int si = SelectedSocket();
    if (si < 0) return;
    const Resource::ModelSocket s = m_sockets[si];   // a copy: the list may change below
    const std::string lock = SocketLock();
    ImGui::PushStyleColor(ImGuiCol_Text, kColSocketText);
    ImGui::Text("Socket  %s", s.name.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(CopySockets({ s.name }).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy it as a socket record (paste onto a bone)");
    if (!lock.empty()) ImGui::TextDisabled("%s", lock.c_str());
    ImGui::BeginDisabled(!lock.empty());

    // Name: committed when the field is left (Enter / focus elsewhere).
    if (m_nameEditFor != si)
    {
        std::snprintf(m_nameEdit, sizeof(m_nameEdit), "%s", s.name.c_str());
        m_nameEditFor = si;
        m_nameError.clear();
    }
    ImGui::InputText("Name", m_nameEdit, sizeof(m_nameEdit));
    if (ImGui::IsItemDeactivatedAfterEdit() && s.name != m_nameEdit)
    {
        Resource::ModelSocket v = s;
        v.name = m_nameEdit;
        std::string why;
        if (SetSocket(s.name, v, why)) { m_nameError.clear(); SelectSocket(v.name); m_nameEditFor = FindSocket(v.name); }
        else { m_nameError = why; std::snprintf(m_nameEdit, sizeof(m_nameEdit), "%s", s.name.c_str()); }
    }
    if (!m_nameError.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", m_nameError.c_str());

    // Bone: searchable (UE's bone picker); Ctrl+click keeps its place.
    if (ImGui::BeginCombo("Bone", s.bone.c_str(), ImGuiComboFlags_HeightLarge))
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##ske_bone_search", "Search bones...", m_boneSearch, sizeof(m_boneSearch));
        const std::string needle = LowerAscii(m_boneSearch);
        // The applied virtual bones first (few, and the reason to look here).
        for (int v = 0; v < static_cast<int>(m_vbs.size()); ++v)
        {
            if (!m_vbProblems[v].empty()) continue;
            if (!needle.empty() && LowerAscii(m_vbs[v].name).find(needle) == std::string::npos) continue;
            ImGui::PushID(0x7300000 + v);
            ImGui::PushStyleColor(ImGuiCol_Text, kColVirtualText);
            if (ImGui::Selectable(m_vbs[v].name.c_str(), m_vbs[v].name == s.bone))
            {
                std::string why;
                if (!ReparentSocket(s.name, m_vbs[v].name, ImGui::GetIO().KeyCtrl, why)) m_error = why;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        for (int b = 0; b < static_cast<int>(m_names.size()); ++b)
        {
            if (!needle.empty() && m_namesLower[b].find(needle) == std::string::npos) continue;
            ImGui::PushID(b);
            if (ImGui::Selectable(m_names[b].c_str(), m_names[b] == s.bone))
            {
                std::string why;
                if (!ReparentSocket(s.name, m_names[b], ImGui::GetIO().KeyCtrl, why)) m_error = why;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Its bone (a virtual bone too). Ctrl+click a bone to keep the socket's place.");

    // T / R / S - one widget gesture, one undo step; R in the Inspector's degrees
    // (XMQuaternionRotationRollPitchYaw), kept while it is this socket's rotation.
    if (m_eulerFor != si || !SameQuat(m_eulerQ, s.q))
    {
        const XMFLOAT3 e = MathUtils::RollPitchYawDegreesFromQuaternion(s.q);
        m_euler[0] = e.x; m_euler[1] = e.y; m_euler[2] = e.z;
        m_eulerFor = si;
        m_eulerQ   = s.q;
    }
    Resource::ModelSocket v = s;
    bool changed = false;
    float t[3] = { s.t.x, s.t.y, s.t.z };
    const float step = std::max(1e-4f, 0.002f * m_modelSize);
    if (ImGui::DragFloat3("Location", t, step, 0.f, 0.f, "%.4f")) { v.t = { t[0], t[1], t[2] }; changed = true; }
    if (ImGui::DragFloat3("Rotation", m_euler, 0.5f, -360.f, 360.f, "%.2f deg"))
    {
        const XMFLOAT4 q = MathUtils::EulerDegreesToQuaternion(XMFLOAT3(m_euler[0], m_euler[1], m_euler[2]));
        v.q = q;
        changed = true;
    }
    float sc[3] = { s.s.x, s.s.y, s.s.z };
    if (ImGui::DragFloat3("Scale", sc, 0.005f, 0.001f, 1000.f, "%.4f"))
    {
        v.s = { std::max(0.001f, sc[0]), std::max(0.001f, sc[1]), std::max(0.001f, sc[2]) };
        changed = true;
    }
    if (changed && Finite(v) && !(v == s))
    {
        if (!m_detailsGesture) { PushUndo(); m_detailsGesture = true; }
        v.source.clear();
        m_sockets[static_cast<std::size_t>(si)] = v;
        m_eulerQ = v.q;   // the degrees typed stay as typed
    }
    if (!ImGui::IsAnyItemActive()) m_detailsGesture = false;
    ImGui::EndDisabled();

    ImGui::Separator();
    if (!s.source.empty()) ImGui::TextDisabled("From the source (%s): a re-import makes it again - an edit makes it yours.",
                                               s.source.c_str());
    if (BindBoneOf(s.bone) < 0)
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Its bone '%s' is not in this skeleton%s.", s.bone.c_str(),
                           s.orphan ? " (an orphan: a re-import lost it)" : "");
    else if (BoneIndex(s.bone) < 0)
        ImGui::TextDisabled("On a virtual bone: where '%s' is, every frame.",
                            m_vbs[static_cast<std::size_t>(FindVirtualBone(s.bone))].to.c_str());
    XMFLOAT4X4 m;
    if (SocketModelMatrix(si, m)) ImGui::TextDisabled("Bind pose (model)  %.4f  %.4f  %.4f", m._41, m._42, m._43);

    ImGui::BeginDisabled(!lock.empty());
    if (ImGui::Button("Delete"))
    {
        std::string why;
        if (!RemoveSocket(s.name, why)) m_error = why;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canAttach = static_cast<bool>(ctx.attachSelection) && lock.empty();
    const bool saved = std::any_of(m_savedSockets.begin(), m_savedSockets.end(), [&](const Resource::ModelSocket& o)
                                   { return o.IsSkeletonSocket() && o.name == s.name; });
    ImGui::BeginDisabled(!canAttach || !saved || ctx.playing);
    if (ImGui::Button("Attach Selected Entity Here"))
    {
        // A component and a scene undo step: the frame boundary's work (PPA).
        m_attachSelQueued = true;
        m_attachSelSocket = s.name;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(!saved ? "Save first: placed instances only have the saved sockets."
                                 : "The main scene's selected entity follows this socket of the placed instance of\n"
                                   "this model nearest to it (one undo step in the scene).");
    ImGui::Separator();
    DrawAttachments(s.name);
}

// The socket's preview attachments: listed (x = remove); a model / prefab
// dropped on the field (or typed) is added. Shown on the Animation host's stage.
void SkeletonEditorWindow::DrawAttachments(const std::string& socket)
{
    ImGui::TextDisabled("Preview assets (the Animation host shows them on this socket)");
    for (std::size_t i = 0; i < m_attachments.size(); ++i)
    {
        const PreviewAttachment& a = m_attachments[i];
        if (a.socket != socket) continue;
        ImGui::PushID(static_cast<int>(i));
        const std::string asset = a.asset;   // the list may change below
        if (ImGui::SmallButton("x"))
        {
            RemoveAttachment(socket, asset);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(BaseName(asset).c_str());
        const std::string err = AttachmentError(i);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s%s", asset.c_str(), err.empty() ? "" : "\n", err.c_str());
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(-60.f);
    ImGui::InputTextWithHint("##ske_attach", "drop a model (.iscn) or a prefab (.ipfb)", m_attachEdit, sizeof(m_attachEdit));
    std::string dropped;
    if (ImGui::BeginDragDropTarget())
    {
        for (const char* type : { "ISCN_PATH", "IPFB_PATH" })
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) dropped = static_cast<const char*>(p->Data);
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add") && m_attachEdit[0]) dropped = m_attachEdit;
    if (!dropped.empty())
    {
        std::string why;
        if (AddAttachment(socket, dropped, why)) m_attachEdit[0] = '\0';
        else m_error = why;
    }
    // The ones whose socket is not in the list (removed; renamed by an undo, a
    // revert or the file): parked below the scene, listed nowhere else.
    bool header = false;
    for (std::size_t i = 0; i < m_attachments.size(); ++i)
    {
        const PreviewAttachment a = m_attachments[i];   // a copy: the list may change below
        if (FindSocket(a.socket) >= 0) continue;
        if (!header)
        {
            ImGui::TextDisabled("On sockets that are gone (parked):");
            header = true;
        }
        ImGui::PushID(0x5A7000 + static_cast<int>(i));
        if (ImGui::SmallButton("x"))
        {
            RemoveAttachment(a.socket, a.asset);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Move here"))
        {
            std::string why;
            if (!MoveAttachment(a.socket, a.asset, socket, why)) m_error = why;
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s  (on '%s')", BaseName(a.asset).c_str(), a.socket.c_str());
        ImGui::PopID();
    }
}

void SkeletonEditorWindow::DrawRenamePopup()
{
    if (m_renameOpen)
    {
        ImGui::OpenPopup("Rename###ske_rename");
        m_renameOpen = false;
    }
    if (!ImGui::BeginPopup("Rename###ske_rename")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##ske_rename_field", m_renameBuf, sizeof(m_renameBuf),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    const bool ok = ImGui::Button("OK") || enter;
    if (ok && m_renameVb && m_renameFor >= 0 && m_renameFor < static_cast<int>(m_vbs.size()))
    {
        // A virtual bone (its sockets follow).
        const std::string old = m_vbs[m_renameFor].name, name = m_renameBuf;
        std::string why;
        if (name == old || RenameVirtualBone(old, name, why))
        {
            SelectVirtualBone(name);
            ImGui::CloseCurrentPopup();
        }
        else m_error = why;
    }
    else if (ok && !m_renameVb && m_renameFor >= 0 && m_renameFor < static_cast<int>(m_sockets.size()))
    {
        const std::string old = m_sockets[m_renameFor].name;
        Resource::ModelSocket v = m_sockets[m_renameFor];
        v.name = m_renameBuf;
        std::string why;
        if (v.name == old || SetSocket(old, v, why))
        {
            SelectSocket(v.name);
            ImGui::CloseCurrentPopup();
        }
        else m_error = why;
    }
    ImGui::EndPopup();
}

void SkeletonEditorWindow::DrawAskPopup(const SkeletonEditorContext& ctx)
{
    if (m_askOpenPopup)
    {
        ImGui::OpenPopup("Unsaved Edits###ske_ask");
        m_askOpenPopup = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved Edits###ske_ask", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("The sockets / virtual bones / humanoid mapping of '%s' have unsaved edits.",
                m_model.empty() ? m_iskel.c_str() : m_model.c_str());
    ImGui::BeginDisabled(ctx.playing);
    if (ImGui::Button("Save"))
    {
        m_saveQueued = true;
        (m_askFor == 1 ? m_closeAfterSave : m_openAfterSave) = true;
        m_askFor = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Discard"))
    {
        if (m_askFor == 1)
        {
            m_sockets = m_savedSockets;
            m_vbs     = m_savedVbs;
            RefreshVirtualBones();
            // Discard the humanoid edits too: a reopened panel keeps its subject, and a
            // later Save would write the discarded mapping.
            if (HumanoidDirty()) RestoreHumanoid(m_savedHumanoid);
            m_undo.clear();
            m_redo.clear();
            m_eulerFor = m_nameEditFor = m_vbNameEditFor = -1;
            m_open = false;
        }
        else
        {
            m_openDiscard = true;
            m_openQueued  = true;
        }
        m_askFor = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        m_askFor = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ==== Virtual bones: the model's VB records, staged with the sockets ====
// The panel's skeleton stays the .iskel's (real bones only); a virtual bone is
// drawn, and resolves a socket's bone, at its `to`'s bind place - where
// ApplyVirtualBones puts it (bind = to's rest model matrix).
int SkeletonEditorWindow::FindVirtualBone(const std::string& name) const
{
    for (int v = 0; v < static_cast<int>(m_vbs.size()); ++v)
        if (m_vbs[v].name == name) return v;
    return -1;
}

std::string SkeletonEditorWindow::VirtualBoneProblem(int index) const
{
    return (index >= 0 && index < static_cast<int>(m_vbProblems.size())) ? m_vbProblems[index]
                                                                          : std::string("no such virtual bone");
}

int SkeletonEditorWindow::VirtualBoneFrom(int index) const
{
    return (index >= 0 && index < static_cast<int>(m_vbFrom.size())) ? m_vbFrom[index] : -1;
}

int SkeletonEditorWindow::VirtualBoneTo(int index) const
{
    return (index >= 0 && index < static_cast<int>(m_vbTo.size())) ? m_vbTo[index] : -1;
}

std::vector<std::string> SkeletonEditorWindow::VirtualBoneVerdicts(
    const std::vector<Resource::ModelVirtualBone>& vbs) const
{
    std::vector<std::string> out(vbs.size());
    if (vbs.empty()) return out;
    if (!m_skel)
    {
        std::fill(out.begin(), out.end(), std::string("no skeleton is loaded"));
        return out;
    }
    // Record by record on ONE working copy - exactly what applying the whole list
    // does (each sees the ones before it): a record is applied iff it appends a bone.
    auto work = std::make_unique<SkeletonAsset>(*m_skel);   // MAX_BONES-sized: heap
    for (std::size_t i = 0; i < vbs.size(); ++i)
    {
        const Resource::ModelVirtualBone& vb = vbs[i];
        if (vb.orphan)
        {
            const std::string lost = BoneIndex(vb.from) < 0 ? vb.from : BoneIndex(vb.to) < 0 ? vb.to : std::string();
            out[i] = lost.empty() ? "marked an orphan by a re-import (orphan=1): kept in the file, never applied"
                                  : "an orphan: a re-import lost its bone '" + lost + "' - kept in the file, never applied";
            continue;
        }
        std::vector<std::string> warns;
        if (Resource::ApplyVirtualBones(*work, { vb }, &warns) == 1) continue;
        // "virtual bone '<name>': <why> - not applied" -> "<why>"
        std::string w = warns.empty() ? std::string("not applied") : warns.front();
        const std::string head = "virtual bone '" + vb.name + "': ", tail = " - not applied";
        if (w.compare(0, head.size(), head) == 0) w.erase(0, head.size());
        if (w.size() > tail.size() && w.compare(w.size() - tail.size(), tail.size(), tail) == 0)
            w.erase(w.size() - tail.size());
        out[i] = w;
    }
    return out;
}

void SkeletonEditorWindow::RefreshVirtualBones()
{
    const std::size_t n = m_vbs.size();
    m_vbFrom.assign(n, -1);
    m_vbTo.assign(n, -1);
    for (std::size_t i = 0; i < n; ++i)
    {
        m_vbFrom[i] = BoneIndex(m_vbs[i].from);
        m_vbTo[i]   = BoneIndex(m_vbs[i].to);
    }
    m_vbProblems = VirtualBoneVerdicts(m_vbs);
    if (m_maskMode >= 0) RefreshMask();   // the mask holds the virtual bones too
}

int SkeletonEditorWindow::BindBoneOf(const std::string& boneName) const
{
    const int b = BoneIndex(boneName);
    if (b >= 0) return b;   // a real bone first - as the runtime's name lookup finds it
    const int v = FindVirtualBone(boneName);
    if (v < 0 || v >= static_cast<int>(m_vbProblems.size()) || !m_vbProblems[v].empty()) return -1;
    return m_vbTo[v];
}

bool SkeletonEditorWindow::PostIKGrant(int b) const
{
    if (!m_skel || b < 0 || b >= static_cast<int>(m_skel->boneCount)) return false;
    if (m_skel->grantSource[b] < 0 || std::fabs(m_skel->grantRatio[b]) < 0.001f) return false;
    std::int32_t minIK = INT32_MAX;
    for (const auto& c : m_skel->ikChains) minIK = std::min<std::int32_t>(minIK, m_skel->transformOrder[c.ikBoneIndex]);
    return m_skel->transformOrder[b] >= minIK;   // AnimationSystem::ApplyPostIKGrants' test
}

std::string SkeletonEditorWindow::UniqueVirtualBoneName(const std::string& base) const
{
    auto used = [&](const std::string& n) { return FindVirtualBone(n) >= 0 || BoneIndex(n) >= 0; };
    const std::string b = Utf8Prefix(base, 63);
    if (!used(b)) return b;
    for (int k = 1; k < 10000; ++k)
    {
        const std::string suffix = "_" + std::to_string(k);
        const std::string n = Utf8Prefix(base, 63 - suffix.size()) + suffix;
        if (!used(n)) return n;
    }
    return b;
}

bool SkeletonEditorWindow::SelectVirtualBone(const std::string& name)
{
    if (name.empty()) { m_selVb.clear(); return true; }
    const int v = FindVirtualBone(name);
    if (v < 0) return false;
    Select(VirtualBoneFrom(v), /*reveal=*/true);   // its row's bone; clears the selection kinds: set it after
    m_selVb          = name;
    m_revealSelected = true;
    LOG_INFO("[SKELEDITOR] select virtual bone -> '%s' (from '%s' to '%s')", name.c_str(), m_vbs[v].from.c_str(),
             m_vbs[v].to.c_str());
    return true;
}

bool SkeletonEditorWindow::AddVirtualBone(const std::string& from, const std::string& to, const std::string& name,
                                          std::string& outName, std::string& why)
{
    if (const std::string lock = SocketLock(); !lock.empty()) { why = lock; return false; }
    for (const std::string* bone : { &from, &to })
        if (BoneIndex(*bone) < 0)
        {
            why = FindVirtualBone(*bone) >= 0 ? "'" + *bone + "' is a virtual bone - a virtual bone joins two real bones"
                                              : "no bone '" + *bone + "' in this skeleton";
            return false;
        }
    if (from == to) { why = "from and to are the same bone"; return false; }
    for (int v = 0; v < static_cast<int>(m_vbs.size()); ++v)
        if (m_vbProblems[v].empty() && m_vbs[v].from == from && m_vbs[v].to == to)
        {
            why = "'" + m_vbs[v].name + "' already joins '" + from + "' to '" + to + "' (one virtual bone per pair)";
            return false;
        }
    Resource::ModelVirtualBone vb;
    vb.name = name.empty() ? UniqueVirtualBoneName(Resource::DefaultVirtualBoneName(from, to)) : name;
    vb.from = from;
    vb.to   = to;
    if (const std::string p = Resource::VirtualBoneNameProblem(vb.name); !p.empty()) { why = p; return false; }
    if (BoneIndex(vb.name) >= 0) { why = "a bone is named '" + vb.name + "'"; return false; }
    if (FindVirtualBone(vb.name) >= 0) { why = "a virtual bone named '" + vb.name + "' exists"; return false; }
    auto next = m_vbs;
    next.push_back(vb);
    // The loader's verdict on the list with it (a full skeleton, ...): what a spawn does.
    if (const std::string p = VirtualBoneVerdicts(next).back(); !p.empty()) { why = p; return false; }
    if (!StageRecords(m_sockets, std::move(next),
                      ("add '" + vb.name + "' ('" + from + "' -> '" + to + "')").c_str(), why))
        return false;
    outName = vb.name;
    return true;
}

bool SkeletonEditorWindow::RenameVirtualBone(const std::string& name, const std::string& newName, std::string& why)
{
    if (const std::string lock = SocketLock(); !lock.empty()) { why = lock; return false; }
    const int v = FindVirtualBone(name);
    if (v < 0) { why = "no virtual bone '" + name + "'"; return false; }
    if (newName == name) return true;
    if (const std::string p = Resource::VirtualBoneNameProblem(newName); !p.empty()) { why = p; return false; }
    if (BoneIndex(newName) >= 0) { why = "a bone is named '" + newName + "'"; return false; }
    if (FindVirtualBone(newName) >= 0) { why = "a virtual bone named '" + newName + "' exists"; return false; }
    auto vbs = m_vbs;
    vbs[static_cast<std::size_t>(v)].name = newName;
    // Its sockets follow (unless the old name is a real bone's: those are that bone's).
    auto sockets = m_sockets;
    std::size_t moved = 0;
    if (BoneIndex(name) < 0)
        for (Resource::ModelSocket& s : sockets)
            if (s.IsSkeletonSocket() && s.bone == name) { s.bone = newName; ++moved; }
    if (!StageRecords(std::move(sockets), std::move(vbs),
                      ("rename '" + name + "' to '" + newName + "' (" + std::to_string(moved) + " socket(s) follow)").c_str(),
                      why))
        return false;
    if (m_selVb == name) m_selVb = newName;
    m_vbNameEditFor = -1;   // the Details' Name field shows the new name
    return true;
}

bool SkeletonEditorWindow::RemoveVirtualBone(const std::string& name, std::string& why)
{
    if (const std::string lock = SocketLock(); !lock.empty()) { why = lock; return false; }
    const int v = FindVirtualBone(name);
    if (v < 0) { why = "no virtual bone '" + name + "'"; return false; }
    const std::string to = m_vbs[static_cast<std::size_t>(v)].to;
    auto vbs = m_vbs;
    vbs.erase(vbs.begin() + v);
    // Its sockets go onto `to` with their offset: where they were in every frame (a
    // virtual bone IS its `to` - before IK).
    auto sockets = m_sockets;
    std::size_t moved = 0;
    if (BoneIndex(name) < 0)
        for (Resource::ModelSocket& s : sockets)
            if (s.IsSkeletonSocket() && s.bone == name) { s.bone = to; ++moved; }
    if (!StageRecords(std::move(sockets), std::move(vbs),
                      ("remove '" + name + "' (" + std::to_string(moved) + " socket(s) onto '" + to + "')").c_str(), why))
        return false;
    if (m_selVb == name) m_selVb.clear();
    return true;
}

// One virtual bone in the tree (under its `from`): select, a socket dropped on it,
// a context menu; its sockets are its child rows.
void SkeletonEditorWindow::DrawVirtualBoneRow(int v)
{
    if (v < 0 || v >= static_cast<int>(m_vbs.size()) || v >= static_cast<int>(m_vbProblems.size())) return;
    const Resource::ModelVirtualBone vb = m_vbs[v];   // a copy: the list may change below
    const bool applied  = m_vbProblems[v].empty();
    static const std::vector<int> kNone;
    const std::vector<int>& socks = v < static_cast<int>(m_socketsByVb.size()) ? m_socketsByVb[v] : kNone;
    const bool selected = m_selSocket.empty() && m_selVb == vb.name;
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                           ImGuiTreeNodeFlags_SpanAvailWidth;
    if (socks.empty()) f |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected) f |= ImGuiTreeNodeFlags_Selected;
    // Reveal: a selected socket on it opens it.
    if (!socks.empty() && m_revealSelected)
        if (const int si = SelectedSocket(); si >= 0 && m_sockets[si].bone == vb.name) ImGui::SetNextItemOpen(true);
    ImGui::PushID(0x7000000 + v);
    ImGui::PushStyleColor(ImGuiCol_Text, applied ? kColVirtualText : kColWarnText);
    const bool open = ImGui::TreeNodeEx("##vb", f, "[v] %s%s", vb.name.c_str(),
                                        vb.orphan ? "  (orphan)" : applied ? "" : "  (not applied)");
    ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) SelectVirtualBone(vb.name);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Virtual bone: a child of '%s' that is where '%s' is, every frame.%s%s\n"
                          "Drop a socket here to put it on the virtual bone.", vb.from.c_str(), vb.to.c_str(),
                          applied ? "" : "\nNot applied: ", applied ? "" : m_vbProblems[v].c_str());
    // A socket dropped here sits on the virtual bone (Ctrl: keeps its place).
    if (applied && SocketLock().empty() && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kSocketPayload))
        {
            const int si = *static_cast<const int*>(p->Data);
            if (si >= 0 && si < static_cast<int>(m_sockets.size()))
            {
                std::string why;
                const std::string sname = m_sockets[si].name;
                if (!ReparentSocket(sname, vb.name, ImGui::GetIO().KeyCtrl, why)) m_error = why;
                else SelectSocket(sname);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##ske_vb_ctx"))
    {
        ImGui::BeginDisabled(!SocketLock().empty());
        ImGui::BeginDisabled(!applied);
        if (ImGui::MenuItem("Add Socket"))
        {
            std::string sn, why;
            if (AddSocket(vb.name, "", sn, why)) SelectSocket(sn);
            else m_error = why;
        }
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Rename", "F2"))
        {
            m_renameFor = v;
            m_renameVb  = true;
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", vb.name.c_str());
            m_renameOpen = true;
        }
        if (ImGui::MenuItem("Delete", "Del"))
        {
            std::string why;
            if (!RemoveVirtualBone(vb.name, why)) m_error = why;
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (selected && m_revealSelected)
    {
        ImGui::SetScrollHereY(0.35f);
        m_revealSelected = false;
    }
    if (open && !socks.empty())
    {
        for (int si : socks) DrawSocketRow(si);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void SkeletonEditorWindow::DrawVirtualBoneTargets(int from)
{
    if (from < 0 || from >= static_cast<int>(m_names.size())) return;
    ImGui::TextDisabled("The bone it follows - its parent is '%s':", m_names[from].c_str());
    if (ImGui::IsWindowAppearing())
    {
        m_vbSearch[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(280.f);
    ImGui::InputTextWithHint("##ske_vb_search", "Search bones...", m_vbSearch, sizeof(m_vbSearch));
    const std::string needle = LowerAscii(m_vbSearch);
    std::vector<int> rows;
    for (int t = 0; t < static_cast<int>(m_names.size()); ++t)
        if (t != from && (needle.empty() || m_namesLower[t].find(needle) != std::string::npos)) rows.push_back(t);
    ImGui::BeginChild("##ske_vb_target_rows", ImVec2(280.f, 300.f), true);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step())
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            const int t = rows[r];
            // One per (from, to) pair among the applied ones (ApplyVirtualBones' rule).
            bool taken = false;
            for (int v = 0; v < static_cast<int>(m_vbs.size()) && !taken; ++v)
                taken = m_vbProblems[v].empty() && m_vbFrom[v] == from && m_vbTo[v] == t;
            ImGui::PushID(t);
            ImGui::BeginDisabled(taken);
            if (ImGui::Selectable(m_names[t].c_str(), false))
            {
                std::string name, why;
                if (AddVirtualBone(m_names[from], m_names[t], "", name, why)) SelectVirtualBone(name);
                else m_error = why;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    if (rows.empty()) ImGui::TextDisabled("No bone matches.");
    ImGui::EndChild();
}

void SkeletonEditorWindow::DrawVirtualBoneDetails(const SkeletonEditorContext& ctx)
{
    (void)ctx;
    const int v = SelectedVirtualBone();
    if (v < 0 || v >= static_cast<int>(m_vbProblems.size())) return;
    const Resource::ModelVirtualBone vb = m_vbs[v];   // a copy: the list may change below
    const std::string lock = SocketLock();
    const bool applied = m_vbProblems[v].empty();
    ImGui::PushStyleColor(ImGuiCol_Text, kColVirtualText);
    ImGui::Text("Virtual Bone  %s", vb.name.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(vb.name.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy its name (a socket's bone, an IK target, Lua Skeleton.*)");
    if (!lock.empty()) ImGui::TextDisabled("%s", lock.c_str());

    // Name: committed when the field is left (its sockets follow).
    ImGui::BeginDisabled(!lock.empty());
    if (m_vbNameEditFor != v)
    {
        std::snprintf(m_vbNameEdit, sizeof(m_vbNameEdit), "%s", vb.name.c_str());
        m_vbNameEditFor = v;
        m_vbNameError.clear();
    }
    ImGui::InputText("Name", m_vbNameEdit, sizeof(m_vbNameEdit));
    if (ImGui::IsItemDeactivatedAfterEdit() && vb.name != m_vbNameEdit)
    {
        const std::string nn = m_vbNameEdit;
        std::string why;
        if (RenameVirtualBone(vb.name, nn, why))
        {
            m_vbNameError.clear();
            SelectVirtualBone(nn);
            m_vbNameEditFor = FindVirtualBone(nn);
        }
        else
        {
            m_vbNameError = why;
            std::snprintf(m_vbNameEdit, sizeof(m_vbNameEdit), "%s", vb.name.c_str());
        }
    }
    if (!m_vbNameError.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", m_vbNameError.c_str());
    ImGui::EndDisabled();

    // Its bones (they do not change - UE's rule: delete it and add another).
    auto link = [&](const char* label, const std::string& bone, int index)
    {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine();
        if (index < 0) { ImGui::TextColored(kColWarnText, "%s (not in this skeleton)", bone.c_str()); return; }
        ImGui::PushID(label);
        if (ImGui::SmallButton(bone.c_str())) Select(index);
        ImGui::PopID();
    };
    link("Parent (from)", vb.from, VirtualBoneFrom(v));
    link("Follows (to) ", vb.to, VirtualBoneTo(v));
    ImGui::TextWrapped("Every frame it is where '%s' is in the animated pose (after the pre-IK 付与, before IK), as "
                       "a child of '%s': IK, HeadTrack or chain physics moving an ancestor of it moves it too.",
                       vb.to.c_str(), vb.from.c_str());
    ImGui::Separator();
    if (!applied)
        ImGui::TextColored(kColWarnText, "Not applied: %s", m_vbProblems[v].c_str());
    else
    {
        const float* p = &m_joints[std::size_t(m_vbTo[v]) * 3];
        ImGui::Text("Bind (model)");
        ImGui::TextDisabled("  %.4f  %.4f  %.4f  (its target's)", p[0], p[1], p[2]);
        if (PostIKGrant(m_vbTo[v]))
            ImGui::TextColored(ImVec4(0.6f, 0.75f, 1.f, 1.f), "'%s' copies a rotation after IK (付与): the virtual bone "
                               "does not have that rotation.", vb.to.c_str());
    }

    // Its sockets.
    ImGui::TextDisabled("Sockets");
    for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
    {
        if (!m_sockets[i].IsSkeletonSocket() || m_sockets[i].bone != vb.name) continue;
        ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, kColSocketText);
        if (ImGui::SmallButton(m_sockets[i].name.c_str())) SelectSocket(m_sockets[i].name);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!lock.empty() || !applied);
    if (ImGui::SmallButton("+ Add Socket"))
    {
        std::string name, why;
        if (AddSocket(vb.name, "", name, why)) SelectSocket(name);
        else m_error = why;
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginDisabled(!lock.empty());
    if (ImGui::Button("Delete"))
    {
        std::string why;
        if (!RemoveVirtualBone(vb.name, why)) m_error = why;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Its sockets stay where they are: on '%s'.", vb.to.c_str());
    ImGui::TextDisabled("A placed instance takes a saved change when it spawns again.");
}

// ==== Compatible animations ====
std::vector<std::string> SkeletonEditorWindow::ListAnimations(const std::string& folder) const
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;   // lower-case paths under the folder
    // The folder, then its twin in the main checkout for a worktree that reads
    // its binary assets from there (Resource/SharedContent.h): it holds no
    // .ianim of its own. A twin's file is listed under THIS folder's path.
    auto walk = [&](const fs::path& root)
    {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (it->is_directory(ec))
            {
                if (it.depth() >= 2) it.disable_recursion_pending();   // the folder and two levels under it
                continue;
            }
            if (!EndsWithCI(Sys::PathToUtf8(it->path()), ".ianim")) continue;
            const fs::path rel = it->path().lexically_relative(root);
            if (seen.insert(LowerAscii(Sys::PathToUtf8(rel))).second)
                out.push_back(Sys::RelativizeAssetPath(Sys::PathToUtf8(Sys::U8Path(folder) / rel)));
        }
    };
    walk(Sys::U8Path(folder));
    if (const std::string twin = Resource::AssetFS::Get().SharedTwinDir(Sys::RelativizeAssetPath(folder)); !twin.empty())
        walk(Sys::U8Path(twin));
    std::sort(out.begin(), out.end());
    return out;
}

void SkeletonEditorWindow::CheckAnimation(const std::string& path)
{
    if (!m_skel) return;
    auto row = [&](const std::string& verdict) { AnimRow r; r.anim = path; r.verdict = verdict; r.bones = m_skel->boneCount; return r; };
    std::vector<std::uint8_t> bytes;
    if (!Resource::AssetFS::Get().ReadFile(path, bytes) || bytes.empty())
    {
        m_animRows.push_back(row("load_failed"));
        return;
    }
    // The loader playback uses, and its binding (Resource::FitClip).
    Resource::AnimationLoader loader;
    std::unique_ptr<Resource::Resource> holder = loader.Load(path, bytes).resource;
    const auto* anim = dynamic_cast<const Resource::AnimationResource*>(holder.get());
    if (!anim) { m_animRows.push_back(row("load_failed")); return; }
    if (anim->clips.empty()) { m_animRows.push_back(row("no_bone_clip")); return; }
    for (const Resource::AnimClipData& clip : anim->clips)
    {
        const Resource::ClipFit f = Resource::FitClip(clip, *m_skel);
        AnimRow r;
        r.anim       = path;
        r.clip       = f.clip;
        r.verdict    = f.Fits() ? "fits" : "rest_pose";
        r.channels   = f.channels;
        r.boundBones = f.boundBones;
        r.bones      = f.bones;
        r.frames     = f.frames;
        r.duration   = f.duration;
        m_animRows.push_back(std::move(r));
    }
}

bool SkeletonEditorWindow::ScanAnimations(const std::string& folderIn, const std::vector<std::string>& files,
                                          std::string& why)
{
    if (!m_loaded || !m_skel) { why = "nothing is open"; return false; }
    std::string folder = folderIn;
    if (folder.empty() && files.empty())
        folder = Sys::PathToUtf8(Sys::U8Path(m_model.empty() ? m_iskel : m_model).parent_path());
    std::vector<std::string> list = folder.empty() ? std::vector<std::string>{} : ListAnimations(folder);
    for (const std::string& f : files)
        if (std::find(list.begin(), list.end(), Sys::RelativizeAssetPath(f)) == list.end())
            list.push_back(Sys::RelativizeAssetPath(f));
    m_animRows.clear();
    m_animQueue.clear();
    m_animTotal   = list.size();
    m_animScanned = folder.empty() ? std::string("the files given") : "'" + Sys::RelativizeAssetPath(folder) + "'";
    for (const std::string& p : list) CheckAnimation(p);
    LOG_INFO("[SKELEDITOR] animations: %zu file(s) of %s checked - %zu row(s)", m_animTotal, m_animScanned.c_str(),
             m_animRows.size());
    return true;
}

void SkeletonEditorWindow::DrawAnimations()
{
    const std::string modelFolder = Sys::PathToUtf8(Sys::U8Path(m_model.empty() ? m_iskel : m_model).parent_path());
    auto queue = [&](const std::string& folder, const std::vector<std::string>& files)
    {
        std::vector<std::string> list = folder.empty() ? std::vector<std::string>{} : ListAnimations(folder);
        for (const std::string& f : files) list.push_back(Sys::RelativizeAssetPath(f));
        m_animRows.clear();
        m_animTotal   = list.size();
        m_animScanned = folder.empty() ? std::string("the file given") : "'" + Sys::RelativizeAssetPath(folder) + "'";
        // Popped from the back: queue reversed so the rows come in name order.
        m_animQueue.assign(list.rbegin(), list.rend());
    };
    // The model's folder, once, the first time the tab shows.
    if (!m_animAutoScanned)
    {
        m_animAutoScanned = true;
        queue(modelFolder, {});
    }
    if (ImGui::Button("Model Folder")) queue(modelFolder, {});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (and two levels under it)", modelFolder.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-90.f);
    ImGui::InputTextWithHint("##ske_anim_folder", "a folder, e.g. asset/Content/animation", m_animFolder,
                             sizeof(m_animFolder));
    ImGui::SameLine();
    if (ImGui::Button("Scan Folder") && m_animFolder[0]) queue(m_animFolder, {});
    ImGui::SetNextItemWidth(-90.f);
    ImGui::InputTextWithHint("##ske_anim_file", "an .ianim", m_animFile, sizeof(m_animFile));
    ImGui::SameLine();
    if (ImGui::Button("Check File") && m_animFile[0]) queue({}, { m_animFile });
    ImGui::Checkbox("Only clips that fit", &m_animOnlyFits);
    ImGui::SameLine();
    if (!m_animQueue.empty())
        ImGui::TextDisabled("checking %zu / %zu ...", m_animTotal - m_animQueue.size(), m_animTotal);
    else
        ImGui::TextDisabled("%zu file(s) of %s", m_animTotal, m_animScanned.c_str());
    ImGui::Separator();
    if (m_animRows.empty())
    {
        ImGui::TextDisabled("%s", m_animQueue.empty() ? "No animation here." : "");
        return;
    }
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##ske_anims", 5, tf)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44.f);   // preview it on the stage
    ImGui::TableSetupColumn("Verdict", ImGuiTableColumnFlags_WidthFixed, 72.f);
    ImGui::TableSetupColumn("Clip");
    ImGui::TableSetupColumn("Bones", ImGuiTableColumnFlags_WidthFixed, 70.f);
    ImGui::TableSetupColumn("File");
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < m_animRows.size(); ++i)
    {
        const AnimRow& r = m_animRows[i];
        if (m_animOnlyFits && r.verdict != "fits") continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // Play it on the Animation host's stage.
        if (r.verdict == "fits" && !m_model.empty())
        {
            ImGui::PushID(static_cast<int>(i));
            const bool current = m_clipPlayer.Path() == r.anim && m_clipPlayer.Source() == r.clip;
            if (ImGui::SmallButton(current ? "(on)" : "Play"))
            {
                RequestClip(r.anim, r.clip);
                if (m_host != Host::Stage && m_stageRefusedFor == m_model) m_stageRefusedFor.clear();
                m_host = Host::Stage;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play it on the model, on the Animation host's live stage.");
            ImGui::PopID();
        }
        ImGui::TableNextColumn();
        const ImVec4 col = r.verdict == "fits"      ? ImVec4(0.45f, 0.9f, 0.45f, 1.f)
                         : r.verdict == "rest_pose" ? ImVec4(1.f, 0.72f, 0.3f, 1.f)
                                                    : ImVec4(0.65f, 0.65f, 0.7f, 1.f);
        ImGui::TextColored(col, "%s", r.verdict.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(r.verdict == "rest_pose" ? "No channel names a bone of this skeleton: it binds, and plays the rest pose."
                              : r.verdict == "no_bone_clip" ? "The file has no bone clip (a camera or morph clip)."
                              : r.verdict == "load_failed"  ? "The file does not read."
                                                            : "It drives this skeleton.");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.clip.empty() ? "-" : r.clip.c_str());
        if (!r.clip.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%u channel(s), %u frame(s), %.2f s", r.channels, r.frames, r.duration);
        ImGui::TableNextColumn();
        if (r.verdict == "fits" || r.verdict == "rest_pose") ImGui::Text("%u / %u", r.boundBones, r.bones);
        else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(BaseName(r.anim).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.anim.c_str());
    }
    ImGui::EndTable();
}

// The Animation host's transport: the clip (the compatible list's clips that
// fit, or the rest pose), step / play / pause, the playhead (a drag pauses),
// speed, loop.
void SkeletonEditorWindow::DrawTransport()
{
    StageClipPlayer& cp = m_clipPlayer;
    // The compatible list, as the Animations tab makes it (the model's folder, once).
    if (!m_animAutoScanned)
    {
        m_animAutoScanned = true;
        const std::string folder = Sys::PathToUtf8(Sys::U8Path(m_model.empty() ? m_iskel : m_model).parent_path());
        const std::vector<std::string> list = ListAnimations(folder);
        m_animRows.clear();
        m_animTotal   = list.size();
        m_animScanned = "'" + Sys::RelativizeAssetPath(folder) + "'";
        m_animQueue.assign(list.rbegin(), list.rend());
    }
    std::string label = "(Rest pose)";
    if (!cp.Path().empty())
        label = (cp.ClipName().empty() ? std::string("...") : cp.ClipName()) + "  -  " + BaseName(cp.Path());
    ImGui::SetNextItemWidth(240.f);
    if (ImGui::BeginCombo("##ske_clip", label.c_str(), ImGuiComboFlags_HeightLarge))
    {
        if (ImGui::Selectable("(Rest pose)", cp.Path().empty())) RequestClip("");
        int shown = 0;
        for (std::size_t i = 0; i < m_animRows.size(); ++i)
        {
            const AnimRow& r = m_animRows[i];
            if (r.verdict != "fits") continue;
            ImGui::PushID(static_cast<int>(i));
            const std::string l = r.clip + "  -  " + BaseName(r.anim);
            if (ImGui::Selectable(l.c_str(), cp.Path() == r.anim && cp.Source() == r.clip)) RequestClip(r.anim, r.clip);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%u / %u bones driven, %.2f s", r.anim.c_str(), r.boundBones, r.bones, r.duration);
            ImGui::PopID();
            ++shown;
        }
        if (!m_animQueue.empty())
            ImGui::TextDisabled("checking %zu / %zu file(s)...", m_animTotal - m_animQueue.size(), m_animTotal);
        else if (shown == 0)
            ImGui::TextDisabled("No clip of %s fits this skeleton (the Animations tab scans elsewhere).",
                                m_animScanned.c_str());
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The clip on the stage (the Animations tab's clips that fit).");
    const bool has = !cp.Path().empty();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has);
    if (ImGui::Button("|<")) { cp.SetPlaying(false); cp.SetTime(0.0); }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##ske_prev", ImGuiDir_Left)) cp.StepFrames(-1);
    ImGui::SameLine();
    if (ImGui::Button(cp.Playing() ? "Pause" : "Play ")) cp.SetPlaying(!cp.Playing());
    ImGui::SameLine();
    if (ImGui::ArrowButton("##ske_next", ImGuiDir_Right)) cp.StepFrames(1);
    ImGui::SameLine();
    float speed = cp.Speed();
    ImGui::SetNextItemWidth(64.f);
    if (ImGui::DragFloat("##ske_speed", &speed, 0.01f, -4.f, 4.f, "x%.2f")) cp.SetSpeed(speed);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speed (negative: backwards)");
    ImGui::SameLine();
    bool loop = cp.Looping();
    if (ImGui::Checkbox("Loop", &loop)) cp.SetLooping(loop);
    // The playhead, its own row: the width is the scrub's precision.
    const float fps = cp.Fps() > 0.f ? cp.Fps() : 30.f;
    char frame[48];
    std::snprintf(frame, sizeof(frame), "f%d / %u%s", static_cast<int>(std::floor(cp.Time() * fps + 1e-4)),
                  cp.Frames(), has && !cp.Bound() ? "  (binding...)" : "");
    float t = static_cast<float>(cp.Time());
    ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(frame).x - 16.f));
    if (ImGui::SliderFloat("##ske_time", &t, 0.f, std::max(cp.Duration(), 1e-3f), "%.3f s"))
    {
        cp.SetPlaying(false);   // a scrub holds the playhead where it is dropped
        cp.SetTime(t);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", frame);
    if (has && ImGui::IsItemHovered())
        ImGui::SetTooltip("%u of %u bones driven, %u channel(s), %.2f s at %.1f fps", cp.BoundBones(),
                          static_cast<unsigned>(m_names.size()), cp.Channels(), cp.Duration(), fps);
}

// ==== The view (AssetPreviewWindow's lifecycle) ====
void SkeletonEditorWindow::UpdateView(const SkeletonEditorContext& ctx)
{
    Renderer* renderer = ctx.renderer;
    m_visibleThisFrame = true;
    if (m_model.empty() || m_releaseQueued) return;
    if (m_viewId == ~0u)
    {
        m_viewId = renderer->AcquirePreviewView(/*worldCull=*/false);
        if (m_viewId == ~0u) return;
    }
    if (m_harnessW != 0) { m_renderW = m_harnessW; m_renderH = m_harnessH; }
    // Nothing is asked for before DrawViewport measured the box the picture goes in
    // (it runs after this, so a new showing asks one frame later). Do not guess a
    // first size: the real one would come through the resize debounce and re-create
    // the view - a frame with no picture, every joint moved.
    if (m_renderW == 0 || m_renderH == 0) return;

    Preview::ViewDesc d{};
    d.w = m_renderW;
    d.h = m_renderH;
    d.source     = Preview::Source::Synthetic;
    d.assetKind  = Preview::AssetKind::Model;
    d.assetPath  = m_model;
    d.background = static_cast<Preview::Background>(m_background);
    d.features   = (d.background != Preview::Background::Solid ? Preview::Feature::kSkybox : 0u)
                 | (m_shadow ? Preview::Feature::kStudioShadow : 0u);
    // 0, NOT 1: the HDR slot's optimised clear value is {0,0,0,0}.
    d.clearColor[0] = d.clearColor[1] = d.clearColor[2] = d.clearColor[3] = 0.f;
    d.yawOffset     = m_yaw;
    d.pitchOffset   = m_pitch;
    d.distanceScale = m_dist;
    d.pan[0] = m_pan[0]; d.pan[1] = m_pan[1]; d.pan[2] = m_pan[2];
    // Hide the mesh: every part masked out (the bones on the backdrop alone).
    d.partMask = m_meshVisible ? ~0ull : 0ull;
    // The weight heatmap's copies in place of the skinned parts (while on).
    m_weights.Apply(d);
    renderer->RequestPreview(m_viewId, d, MaterialComponent{});

    char key[400];
    std::snprintf(key, sizeof(key), "%s|%u|%u|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%d|%d|%d|%llu", m_model.c_str(), d.w, d.h,
                  d.yawOffset, d.pitchOffset, d.distanceScale, d.pan[0], d.pan[1], d.pan[2], m_background,
                  m_shadow ? 1 : 0, m_meshVisible ? 1 : 0,
                  static_cast<unsigned long long>(m_weights.Active() ? m_weights.Revision() : 0));
    if (m_lastDescKey != key)
    {
        m_lastDescKey   = key;
        m_rendersAtDesc = renderer->GetPreviewViewRenderCount(m_viewId);
    }
}

void SkeletonEditorWindow::ProcessPendingActions(const SkeletonEditorContext& ctx)
{
    // AUTO-RELEASE at the frame boundary: this runs BEFORE this frame's Draw,
    // so the flag is the previous frame's answer - "the panel did not draw".
    if (m_viewId != ~0u && !m_visibleThisFrame) m_releaseQueued = true;
    if (!m_visibleThisFrame)
    {
        ReleaseGizmo();   // a gizmo drag in a panel that stopped drawing
        // Its box is measured again when it next draws (a docked tab may come back
        // at another size): UpdateView asks for nothing until then.
        m_renderW = m_renderH = m_pendW = m_pendH = 0;
    }
    m_visibleThisFrame = false;

    // The weight heatmap follows the selected bone: its copies made, recoloured or
    // handed back with the panel (the heap only takes calls from this half).
    if (ctx.renderer && ctx.gfx)
    {
        MeshDescriptorHeap& heap = ctx.renderer->GetMeshManager().GetDescriptorHeap();
        // A selected virtual bone has no weights: not its `from`'s either.
        m_weights.SetBone(m_weightsOn && m_selVb.empty() ? m_selected : -1);
        if (m_loaded && m_open) m_weights.Update(*ctx.gfx, heap, ctx.worldEpoch);
        else if (m_weights.HasGpu()) m_weights.Release(ctx.gfx, &heap, ctx.worldEpoch);
    }
    // The animation list's scan, a few files a frame.
    if (!m_animQueue.empty())
    {
        for (int k = 0; k < 4 && !m_animQueue.empty(); ++k)
        {
            const std::string path = m_animQueue.back();
            m_animQueue.pop_back();
            CheckAnimation(path);
        }
        if (m_animQueue.empty())
            LOG_INFO("[SKELEDITOR] animations: %zu file(s) of %s checked - %zu row(s)", m_animTotal,
                     m_animScanned.c_str(), m_animRows.size());
    }

    // The panel's own history and Save, queued from the UI.
    for (; m_pendingUndo > 0; --m_pendingUndo) Undo();
    for (; m_pendingRedo > 0; --m_pendingRedo) Redo();
    if (m_saveQueued)
    {
        m_saveQueued = false;
        std::string why;
        const bool forceHumanoid = m_forceHumanoidSave;   // the Retarget tab's "Save to .iskel"
        m_forceHumanoidSave = false;
        if (SaveNow(ctx, why, nullptr, forceHumanoid))
        {
            m_error.clear();
            if (m_closeAfterSave) m_open = false;
            if (m_openAfterSave) m_openQueued = true;
        }
        else m_error = why;
        m_closeAfterSave = m_openAfterSave = false;
    }
    // The socket Details' "Attach Selected Entity Here", queued by Draw.
    if (m_attachSelQueued)
    {
        m_attachSelQueued = false;
        if (ctx.attachSelection && m_loaded && !m_model.empty() && !ctx.playing)
        {
            const std::string why = ctx.attachSelection(m_model, m_attachSelSocket);
            if (why.empty()) m_lastSave = "Attached the selected entity to '" + m_attachSelSocket + "'";
            else             m_error = why;
        }
    }

    if (ctx.hasWorld)
    {
        if (m_worldEpochSeen && ctx.worldEpoch != m_worldEpoch && m_loaded) m_recomposeQueued = true;
        m_worldEpoch     = ctx.worldEpoch;
        m_worldEpochSeen = true;
    }
    if (m_recomposeQueued && ctx.renderer)
    {
        m_recomposeQueued = false;
        if (!m_model.empty()) ctx.renderer->InvalidatePreviewAsset(m_model);
    }
    if (m_openQueued)
    {
        m_openQueued = false;
        if (Dirty() && !m_openDiscard)
        {
            // Unsaved edits: ask (Save / Discard / Cancel) before opening another.
            if (m_askFor == 0) { m_askFor = 2; m_askOpenPopup = true; }
        }
        else
        {
            m_openDiscard = false;
            std::string code, why;
            if (!LoadSubject(ctx, m_queuedPath, code, why))
            {
                m_error = why;
                LOG_WARNING("[SKELEDITOR] open '%s' failed (%s): %s", m_queuedPath.c_str(), code.c_str(), why.c_str());
            }
            // An AnimLayer's "Preview" asked for its mask on this model.
            if (m_maskQueued && m_loaded && LowerAscii(m_maskQueuedModel) == LowerAscii(m_queuedPath))
                SetMask(m_maskQueuedMode, m_maskQueuedRoot);
            m_maskQueued = false;
        }
    }
    // "Open Animation Retarget with this target": it selects a placement (the
    // World).
    if (m_retargetQueued)
    {
        m_retargetQueued = false;
        m_retargetMsg = !m_loaded || m_model.empty() ? std::string("no model: the retarget writes onto a placed model")
                      : ctx.openRetarget ? ctx.openRetarget(m_model) : std::string("the editor did not wire this");
        if (!m_retargetMsg.empty()) LOG_INFO("[SKELEDITOR] retarget: %s", m_retargetMsg.c_str());
    }

    // The model file's sockets or virtual bones changed on disk (a re-import,
    // another tool): checked every 30 editor frames (the ...At caches re-read only
    // when the file did). A clean panel follows the file; a dirty one keeps its
    // edits, and Revert takes the file's new records, Save overwrites them. The
    // placed instances without the file's virtual bones are counted then too.
    if (m_loaded && !m_model.empty() && !m_gizmoUsing && ctx.editorFrame % 30 == 0)
    {
        // The skeleton sockets only (a Mesh Editor save of a Mesh's is not this panel's).
        const std::vector<Resource::ModelSocket> onDisk = SkeletonSocketsOf(Resource::ModelSocketsAt(m_model)->sockets);
        const std::vector<Resource::ModelVirtualBone>& vbOnDisk = Resource::ModelVirtualBonesAt(m_model)->bones;
        if (onDisk != m_savedSockets || vbOnDisk != m_savedVbs)
        {
            // Only the records' edits decide: a humanoid-only edit must not keep stale
            // records as edits, or Save writes them back over a re-import's.
            const bool dirty = SocketsDirty() || VirtualBonesDirty();
            m_savedSockets = onDisk;
            m_savedVbs     = vbOnDisk;
            if (!dirty)
            {
                m_sockets = onDisk;
                m_vbs     = vbOnDisk;
                RefreshVirtualBones();
                m_undo.clear();
                m_redo.clear();
                m_eulerFor = m_nameEditFor = m_vbNameEditFor = -1;
            }
            else
                m_error = "The model file's sockets / virtual bones changed on disk: Save overwrites them, Revert takes them.";
            LOG_INFO("[SKELEDITOR] '%s' changed on disk: %zu socket record(s), %zu virtual bone(s) - %s", m_model.c_str(),
                     onDisk.size(), vbOnDisk.size(), dirty ? "the staged edits kept" : "reloaded");
        }
        if (ctx.staleVirtualBones) m_staleInstances = ctx.staleVirtualBones(m_model);
    }
    // The .iskel changed on disk (a re-import, the Animation Retarget window's
    // Save): a clean panel opens it again (its bones may have moved); a dirty one
    // says so once - its humanoid Save refuses the changed file.
    if (m_loaded && !m_iskel.empty() && !m_gizmoUsing && !m_openQueued && ctx.editorFrame % 30 == 15)
    {
        const std::string stamp = FileStamp(m_iskel);
        if (!stamp.empty() && stamp != m_iskelStamp)
        {
            if (!Dirty())
            {
                LOG_INFO("[SKELEDITOR] '%s' changed on disk: opened again", m_iskel.c_str());
                m_iskelStamp  = stamp;
                m_queuedPath  = m_path;
                m_openDiscard = true;
                m_openQueued  = true;
            }
            else if (!m_iskelChangedSaid)
            {
                m_iskelChangedSaid = true;
                m_error = "The skeleton file (.iskel) changed on disk: the humanoid mapping cannot be saved over it - "
                          "Save the records, then reopen.";
                LOG_WARNING("[SKELEDITOR] '%s' changed on disk with unsaved edits staged", m_iskel.c_str());
            }
        }
    }

    TickStage(ctx);   // the Animation host's stage (spawns / destroys entities: here only)
    TickHarness(ctx);

    if (!m_releaseQueued) return;
    m_releaseQueued = false;
    if (m_viewId != ~0u && ctx.renderer)
    {
        ctx.renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[SKELEDITOR] released view %u at the frame boundary", m_viewId);
    }
    m_viewId = ~0u;
}

void SkeletonEditorWindow::ReleaseViewsAtShutdown(Renderer* renderer)
{
    // The device is going away with everything it holds: forget the copies.
    m_weights.Release(nullptr, nullptr, 0);
    // The stage's view goes, its pin with it; its entities are left to the World's
    // own teardown (no World here: the ids are only dropped).
    m_stage.TearDown(renderer, nullptr, "shutdown");
    m_attachLive.clear();
    m_live.valid = false;
    m_releaseQueued = false;
    if (m_viewId == ~0u) return;
    if (renderer)
    {
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[SKELEDITOR] released view %u at shutdown", m_viewId);
    }
    m_viewId = ~0u;
}

// ==== The Animation host's stage ====
// The model in the main World at origin slot 1, seen by a WorldCull view.
// Editor/PreviewStage does the spawn (strip, tags, view policy, budget), the
// pin, the view and the camera; this decides when (the panel shows the Stage
// host, in Edit mode) and frames it.
bool SkeletonEditorWindow::StageMode(bool playing) const
{
    return m_host == Host::Stage && m_loaded && !m_model.empty() && !playing && m_stageRefusedFor != m_model;
}

bool SkeletonEditorWindow::StageShown() const
{
    return m_host == Host::Stage && m_stage.Up() && m_stage.View() != ~0u;
}

std::uint32_t SkeletonEditorWindow::CurrentView() const
{
    return StageShown() ? m_stage.View() : m_viewId;
}

void SkeletonEditorWindow::TearDownStage(const SkeletonEditorContext& ctx, const char* reason)
{
    m_stage.TearDown(ctx.renderer, ctx.world, reason);
    m_live.valid = false;
    m_attachLive.clear();   // adopted by the stage - gone with it
    m_attachDirty = true;
}

void SkeletonEditorWindow::TearDownForPlay(Renderer* renderer, World* world)
{
    if (!m_stage.Up() && m_stage.View() == ~0u) return;
    // Before CloneState: a stage alive here would be baked into the Play snapshot
    // and AdoptState would bring it back at Stop owned by nothing. The panel shows
    // the reference pose until Stop; the next Edit-mode frame that shows the Stage
    // host spawns it again.
    m_stage.TearDown(renderer, world, "enter play");
    m_attachLive.clear();   // went with the stage
    m_attachDirty = true;
    m_live.valid = false;
}

void SkeletonEditorWindow::RequestClip(const std::string& path, const std::string& clip)
{
    m_clipRequested = true;
    m_clipReqPath   = path;
    m_clipReqName   = clip;
    // A picked clip starts at its beginning, playing (a script's time / playing given in
    // the same call are applied after this).
    m_clipPlayer.SetTime(0.0);
    m_clipPlayer.SetPlaying(true);
}

void SkeletonEditorWindow::TickStage(const SkeletonEditorContext& ctx)
{
    Renderer* renderer = ctx.renderer;
    World*    world    = ctx.world;
    // A clip picked (the transport's combo, the Animations tab, Lua): the player
    // keeps it across stage respawns.
    if (m_clipRequested)
    {
        m_clipRequested = false;
        m_clipPlayer.Select(ctx.animClipSys, m_clipReqPath, m_clipReqName);
    }
    // Draw's answer from the previous frame: the panel showed the Stage host.
    const bool drawn = m_stageDrawn;
    m_stageDrawn = false;
    // A world clear / adopt (a scene load, Stop): the ids name nothing now - dropped,
    // never destroyed; the next tick spawns the stage again when it is still wanted.
    if (world && (world->Epoch() != m_stageEpoch || world != m_stageWorld))
    {
        if (m_stage.Up())
        {
            LOG_INFO("[SKELEDITOR] world epoch changed: the stage's ids dropped (root %u)",
                     static_cast<unsigned>(m_stage.Root()));
            m_stage.ForgetEntities();
            m_attachLive.clear();   // their ids went with the world
            m_attachDirty = true;
            m_live.valid = false;
        }
        m_stageEpoch = world->Epoch();
        m_stageWorld = world;
    }
    // -skeleton_editor_stage_off (reverse arm): the Animation host never gets its stage.
    static const bool s_stageOff = Sys::CommandLine::Get().Has("-skeleton_editor_stage_off");
    if (s_stageOff && m_host == Host::Stage) m_stageStatus = "The stage is off (-skeleton_editor_stage_off).";
    const bool want = m_open && drawn && StageMode(ctx.playing) && renderer && world && ctx.assetMgr &&
                      renderer->IsPreviewEnabled() && !s_stageOff;
    if (!want)
    {
        if (m_stage.Up() || m_stage.View() != ~0u)
            TearDownStage(ctx, ctx.playing ? "play" : m_host != Host::Stage ? "reference host" : "not shown");
        return;
    }
    if (m_stage.Up() && !m_stage.Validate(*world))
    {
        TearDownStage(ctx, "destroyed outside");
        m_stageRefusedFor = m_model;
        m_stageStatus = "The stage was deleted outside the panel - switch the host (or reopen) to spawn it again.";
        return;
    }
    if (m_stage.Up() && m_stageRespawn)
    {
        TearDownStage(ctx, "virtual bones saved");
        m_stageRespawn = false;
    }
    // One view at a time: the reference host's goes before the stage's comes.
    if (m_viewId != ~0u)
    {
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[SKELEDITOR] released view %u (stage host)", m_viewId);
        m_viewId = ~0u;
        m_releaseQueued = false;
    }
    if (!m_stage.Up())
    {
        std::string why;
        if (!m_stage.Spawn(*world, *renderer, *ctx.assetMgr, ctx.animClipSys, m_model, why))
        {
            m_stageRefusedFor = m_model;
            m_stageStatus = "No stage: " + why;
            LOG_WARNING("[SKELEDITOR] stage refused for '%s': %s", m_model.c_str(), why.c_str());
            return;
        }
        m_stage.Pin(*renderer);
        m_stageStatus.clear();
        m_stagePosed = m_stageRest = 0;
        m_stageTicks = 0;
        m_clipPlayer.OnNewStage();   // the clip's alias onto the new stage's Animator
        m_stageSocketsDerived = SkeletonSocketsOf(Resource::ModelSocketsAt(m_model)->sockets);   // what the load derived
        m_attachDirty = true;   // the preview attachments onto the new stage
        m_poseDirty   = true;   // and the temporary pose
        m_live.valid = false;
        LOG_INFO("[SKELEDITOR] stage pinned: skeleton=%u owners=%zu", static_cast<unsigned>(m_stage.Skeleton()),
                 renderer->AnimPinOwnerCount());
    }
    // The clip: the alias on the stage's Animator, the playhead, the clock written
    // (sampled by the next Animation phase). Even without a view: the pose is the
    // stage's.
    const SkeletonComponent* stageSk = world->GetComponent<SkeletonComponent>(m_stage.Skeleton());
    {
        const SkeletonAsset* stageSkel = nullptr;
        SkeletonRegistry& reg = renderer->GetSkeletonRegistry();
        if (stageSk && stageSk->assetIndex < reg.Count()) stageSkel = &reg.Get(stageSk->assetIndex);
        m_clipPlayer.Update(*world, ctx.animClipSys, Anim::ResolveAnimEntityOrSelf(*world, m_stage.Skeleton()),
                            stageSkel, ctx.realDt);
        // The stage's model sockets are the staged records (the load derived the
        // file's), derived again whenever the list changed, so what follows a socket (a
        // preview attachment) follows its edits at once.
        if (stageSkel && m_stageSocketsDerived != m_sockets)
        {
            Resource::ModelSockets staged;
            staged.sockets = m_sockets;
            SceneInstanceLoader::DeriveModelSockets(*world, m_stage.Root(), *stageSkel, staged, m_model);
            m_stageSocketsDerived = m_sockets;
        }
    }
    TickAttachments(ctx);   // the preview attachments (rebuilt when the list / the stage changed)
    ParkAttachments(ctx);   // one whose socket the stage lacks waits below the scene
    TickPose(ctx);          // the temporary pose onto the stage's skeleton
    if (!m_stage.EnsureView(*renderer)) return;
    m_stage.EnsureCamera(*world);
    // The pin's read-out: a pose-ring slot this frame (poseByteOffset == ~0u is
    // LocalToWorldSystem's "anim-culled: no pose" sentinel), counted from the
    // stage's third tick: the spawn frame's classify ran before the pin was pushed.
    // Fetched again: an attachment spawned above (a skinned one) grows the
    // SkeletonComponent pool, so the pointer taken before may dangle.
    stageSk = world->GetComponent<SkeletonComponent>(m_stage.Skeleton());
    if (++m_stageTicks > 2 && stageSk) (stageSk->poseByteOffset == ~0u ? m_stageRest : m_stagePosed) += 1;
    m_stage.NegotiateSize(*renderer, m_renderW, m_renderH);
    BuildLiveFrame(ctx);
    m_stage.Request(*renderer);
}

// The camera (the reference host's orbit - yaw / pitch / zoom / pan - around the
// model's centre, at the model's size: the studio rig frames a unit-diameter
// subject) and the live frame the overlay reads.
void SkeletonEditorWindow::BuildLiveFrame(const SkeletonEditorContext& ctx)
{
    World& world = *ctx.world;
    // The root's world matrix from its LocalTransform (the stage root has no parent
    // and is placed this frame - GlobalTransform catches up after the transform pass).
    XMMATRIX root = XMMatrixTranslation(0.f, m_stage.OriginY(), 0.f);
    if (const LocalTransform* lt = world.GetComponent<LocalTransform>(m_stage.Root()))
        root = XMMatrixAffineTransformation(XMLoadFloat3(&lt->scale), XMVectorZero(), XMLoadFloat4(&lt->rotation),
                                            XMLoadFloat3(&lt->translation));
    const float size = std::max(1e-3f, m_modelSize);
    const XMVECTOR centre = XMVector3TransformCoord(
        XMVectorSet(0.5f * (m_boundsMin[0] + m_boundsMax[0]), 0.5f * (m_boundsMin[1] + m_boundsMax[1]),
                    0.5f * (m_boundsMin[2] + m_boundsMax[2]), 1.f), root);
    const float pan[3] = { m_pan[0] * size, m_pan[1] * size, m_pan[2] * size };
    const bool panned = pan[0] != 0.f || pan[1] != 0.f || pan[2] != 0.f;
    float eye[3], fwd[3];
    Preview::StudioCameraBasis(m_yaw, m_pitch, m_dist * size, eye, fwd, panned ? pan : nullptr);
    const float eyeW[3] = { XMVectorGetX(centre) + eye[0], XMVectorGetY(centre) + eye[1], XMVectorGetZ(centre) + eye[2] };
    m_stage.PlaceCamera(world, eyeW, fwd);

    // The projection the stage's view renders with (its camera entity's lens, the
    // view's size; reversed-Z like every preview camera - x / y do not care).
    const std::uint32_t w = m_stage.RenderW(), h = m_stage.RenderH();
    const XMMATRIX view = XMMatrixLookToLH(XMVectorSet(eyeW[0], eyeW[1], eyeW[2], 1.f),
                                           XMVectorSet(fwd[0], fwd[1], fwd[2], 0.f), XMVectorSet(0.f, 1.f, 0.f, 0.f));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(Preview::kStudioCamera.fovY,
                                                   h ? static_cast<float>(w) / static_cast<float>(h) : 1.f,
                                                   kStageCameraFar, kStageCameraNear);
    XMStoreFloat4x4(&m_live.view, view);
    XMStoreFloat4x4(&m_live.proj, proj);
    XMStoreFloat4x4(&m_live.rootWorld, root);
    m_live.w = w;
    m_live.h = h;

    // The live bones: the ONE FK every overlay reads (EditorLayer::BuildBoneWorldMatrices).
    std::vector<XMFLOAT4X4> b2m;
    XMMATRIX ent = XMMatrixIdentity();
    if (!ctx.buildBones || !ctx.buildBones(*ctx.renderer, world, m_stage.Skeleton(), b2m, ent))
    {
        m_live.valid = false;
        return;
    }
    // The panel's bones are the .iskel's real ones: the stage's first N (its virtual
    // bones come after them).
    const std::size_t n = std::min(b2m.size(), m_names.size());
    m_live.joints.assign(m_names.size() * 3, 0.f);
    m_live.axes.assign(m_names.size() * 9, 0.f);
    m_live.boneWorld.assign(m_names.size(), XMFLOAT4X4{});
    for (std::size_t b = 0; b < n; ++b)
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixMultiply(XMLoadFloat4x4(&b2m[b]), ent));
        m_live.boneWorld[b] = m;
        m_live.joints[b * 3 + 0] = m._41;
        m_live.joints[b * 3 + 1] = m._42;
        m_live.joints[b * 3 + 2] = m._43;
        const float rows[3][3] = { { m._11, m._12, m._13 }, { m._21, m._22, m._23 }, { m._31, m._32, m._33 } };
        for (int r = 0; r < 3; ++r)
        {
            const float len = std::sqrt(rows[r][0] * rows[r][0] + rows[r][1] * rows[r][1] + rows[r][2] * rows[r][2]);
            for (int c = 0; c < 3; ++c) m_live.axes[b * 9 + r * 3 + c] = len > 1e-8f ? rows[r][c] / len : (r == c ? 1.f : 0.f);
        }
    }
    m_live.valid = n == m_names.size() && w != 0 && h != 0;

    // The selected bone's animated local pose (the pose cache AnimationSystem
    // wrote), copied here in the serial window; Details reads only the copy.
    m_live.selBone  = m_selected;
    m_live.selValid = false;
    if (m_selected >= 0 && m_selected < static_cast<int>(m_names.size()))
        if (AnimationSystem* as = ctx.renderer->GetAnimationSystem())
            if (const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(m_stage.Skeleton());
                sk && static_cast<std::uint32_t>(m_selected) < sk->boneCount)
                if (const AnimationSystem::LocalPose* lp = as->GetLocalPose(m_stage.Skeleton()))
                {
                    m_live.selPos   = lp[m_selected].pos;
                    m_live.selRot   = lp[m_selected].rot;
                    m_live.selScl   = lp[m_selected].scl;
                    m_live.selValid = true;
                }
}

// ==== Preview attachments: a model / prefab on a socket of the stage ====
std::string SkeletonEditorWindow::AttachmentsFile() const
{
    // Per model, per user: the lower-cased model path, FNV-1a 64 (the file names it too).
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : LowerAscii(Sys::RelativizeAssetPath(m_model))) { h ^= c; h *= 1099511628211ull; }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.json", static_cast<unsigned long long>(h));
    return Sys::WritableDataDir() + "/cache/skeleton_editor/" + name;
}

void SkeletonEditorWindow::LoadAttachments()
{
    m_attachments.clear();
    m_attachDirty = true;
    if (m_model.empty()) return;
    std::ifstream f(Sys::U8Path(AttachmentsFile()), std::ios::binary);
    if (!f) return;
    try
    {
        const nlohmann::json j = nlohmann::json::parse(f);
        for (const auto& a : j.value("attachments", nlohmann::json::array()))
            if (a.is_object() && a.contains("socket") && a.contains("asset"))
                m_attachments.push_back({ a["socket"].get<std::string>(), a["asset"].get<std::string>() });
    }
    catch (const std::exception& e)
    {
        LOG_WARNING("[SKELEDITOR] preview attachments '%s' do not read (%s) - none", AttachmentsFile().c_str(), e.what());
        m_attachments.clear();
    }
}

void SkeletonEditorWindow::SaveAttachments() const
{
    if (m_model.empty()) return;
    const std::string path = AttachmentsFile();
    std::error_code ec;
    std::filesystem::create_directories(Sys::U8Path(path).parent_path(), ec);
    nlohmann::json j;
    j["model"] = m_model;
    j["attachments"] = nlohmann::json::array();
    for (const PreviewAttachment& a : m_attachments) j["attachments"].push_back({ { "socket", a.socket }, { "asset", a.asset } });
    std::ofstream f(Sys::U8Path(path), std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(2);
    if (!f) LOG_WARNING("[SKELEDITOR] preview attachments '%s' were not written", path.c_str());
}

bool SkeletonEditorWindow::AddAttachment(const std::string& socket, const std::string& assetIn, std::string& why)
{
    if (!m_loaded || m_model.empty()) { why = "no model is open"; return false; }
    const std::string asset = Sys::RelativizeAssetPath(assetIn);
    if (!EndsWithCI(asset, ".iscn") && !EndsWithCI(asset, ".ipfb"))
    {
        why = "a preview attachment is a model (.iscn) or a prefab (.ipfb): '" + asset + "'";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(Sys::U8Path(asset), ec)) { why = "no such file: '" + asset + "'"; return false; }
    if (FindSocket(socket) < 0) { why = "no socket '" + socket + "'"; return false; }
    const PreviewAttachment a{ socket, asset };
    if (std::find(m_attachments.begin(), m_attachments.end(), a) != m_attachments.end())
    {
        why = "'" + asset + "' is on '" + socket + "' already";
        return false;
    }
    m_attachments.push_back(a);
    m_attachDirty = true;
    SaveAttachments();
    LOG_INFO("[SKELEDITOR] preview attachment added: '%s' on '%s' (%zu)", asset.c_str(), socket.c_str(),
             m_attachments.size());
    return true;
}

bool SkeletonEditorWindow::RemoveAttachment(const std::string& socket, const std::string& asset)
{
    const std::size_t before = m_attachments.size();
    std::erase_if(m_attachments, [&](const PreviewAttachment& a)
                  { return a.socket == socket && (asset.empty() || a.asset == Sys::RelativizeAssetPath(asset)); });
    if (m_attachments.size() == before) return false;
    m_attachDirty = true;
    SaveAttachments();
    LOG_INFO("[SKELEDITOR] preview attachment(s) removed from '%s': %zu left", socket.c_str(), m_attachments.size());
    return true;
}

bool SkeletonEditorWindow::MoveAttachment(const std::string& socket, const std::string& assetIn,
                                          const std::string& toSocket, std::string& why)
{
    const std::string asset = Sys::RelativizeAssetPath(assetIn);
    if (FindSocket(toSocket) < 0) { why = "no socket '" + toSocket + "'"; return false; }
    const auto it = std::find(m_attachments.begin(), m_attachments.end(), PreviewAttachment{ socket, asset });
    if (it == m_attachments.end()) { why = "no preview attachment '" + asset + "' on '" + socket + "'"; return false; }
    if (socket == toSocket) return true;
    if (std::find(m_attachments.begin(), m_attachments.end(), PreviewAttachment{ toSocket, asset }) != m_attachments.end())
    {
        why = "'" + asset + "' is on '" + toSocket + "' already";
        return false;
    }
    it->socket    = toSocket;
    m_attachDirty = true;
    SaveAttachments();
    LOG_INFO("[SKELEDITOR] preview attachment '%s' moved from '%s' onto '%s'", asset.c_str(), socket.c_str(),
             toSocket.c_str());
    return true;
}

void SkeletonEditorWindow::CarryAttachments(const std::vector<Resource::ModelSocket>& before,
                                            const std::vector<Resource::ModelSocket>& after)
{
    if (m_attachments.empty()) return;
    auto named = [](const std::vector<Resource::ModelSocket>& list, const std::string& n)
    {
        return std::any_of(list.begin(), list.end(),
                           [&](const Resource::ModelSocket& s) { return s.IsSkeletonSocket() && s.name == n; });
    };
    std::vector<std::string> gone, came;
    for (const Resource::ModelSocket& s : before)
        if (s.IsSkeletonSocket() && !named(after, s.name)) gone.push_back(s.name);
    for (const Resource::ModelSocket& s : after)
        if (s.IsSkeletonSocket() && !named(before, s.name)) came.push_back(s.name);
    if (gone.size() != 1 || came.size() != 1) return;   // not one rename: nothing follows
    std::size_t moved = 0;
    for (PreviewAttachment& a : m_attachments)
    {
        if (a.socket != gone[0]) continue;
        const PreviewAttachment to{ came[0], a.asset };
        if (std::find(m_attachments.begin(), m_attachments.end(), to) != m_attachments.end()) continue;
        a.socket = came[0];
        ++moved;
    }
    if (moved == 0) return;
    m_attachDirty = true;
    SaveAttachments();
    LOG_INFO("[SKELEDITOR] %zu preview attachment(s) followed the socket '%s' -> '%s'", moved, gone[0].c_str(),
             came[0].c_str());
}

Entity SkeletonEditorWindow::AttachmentRoot(std::size_t index) const
{
    if (index >= m_attachments.size()) return NullEntity;
    for (const AttachmentLive& l : m_attachLive)
        if (l.what == m_attachments[index]) return l.root;
    return NullEntity;
}

std::string SkeletonEditorWindow::AttachmentError(std::size_t index) const
{
    if (index >= m_attachments.size()) return {};
    for (const AttachmentLive& l : m_attachLive)
        if (l.what == m_attachments[index])
            return !l.error.empty() ? l.error
                 : l.parked ? "parked: its socket is not on the stage (removed or renamed, or on a virtual bone "
                              "that is not saved yet)"
                            : std::string();
    return {};
}

bool SkeletonEditorWindow::AttachmentParked(std::size_t index) const
{
    if (index >= m_attachments.size()) return false;
    for (const AttachmentLive& l : m_attachLive)
        if (l.what == m_attachments[index]) return l.parked;
    return false;
}

// ==== Temporary posing: PreviewPoseOverrideComponent on the stage ====
void SkeletonEditorWindow::SetBonePose(int bone, const XMFLOAT3& deg)
{
    if (bone < 0 || bone >= static_cast<int>(m_names.size())) return;
    std::erase_if(m_poseEdits, [bone](const auto& p) { return p.first == bone; });
    if (deg.x != 0.f || deg.y != 0.f || deg.z != 0.f) m_poseEdits.push_back({ bone, deg });
    m_poseDirty = true;
}

void SkeletonEditorWindow::ResetPose()
{
    if (m_poseEdits.empty()) return;
    m_poseEdits.clear();
    m_poseDirty = true;
    LOG_INFO("[SKELEDITOR] pose reset");
}

// The component on the stage's skeleton, whenever the edits (or the stage) changed.
void SkeletonEditorWindow::TickPose(const SkeletonEditorContext& ctx)
{
    if (!m_poseDirty || !m_stage.Up() || !ctx.world) return;
    m_poseDirty = false;
    World& world = *ctx.world;
    const Entity skel = m_stage.Skeleton();
    if (m_poseEdits.empty())
    {
        if (world.HasComponent<PreviewPoseOverrideComponent>(skel)) world.RemoveComponent<PreviewPoseOverrideComponent>(skel);
        return;
    }
    PreviewPoseOverrideComponent pc;
    for (const auto& [bone, deg] : m_poseEdits)
        pc.entries.push_back({ static_cast<std::uint32_t>(bone), MathUtils::EulerDegreesToQuaternion(deg) });
    if (PreviewPoseOverrideComponent* have = world.GetComponent<PreviewPoseOverrideComponent>(skel)) *have = std::move(pc);
    else world.AddComponent<PreviewPoseOverrideComponent>(skel, std::move(pc));
    LOG_INFO("[SKELEDITOR] pose: %zu bone(s) turned on the stage (skeleton %u)", m_poseEdits.size(),
             static_cast<unsigned>(skel));
}

// The stage's attachments, rebuilt whenever the list (or the stage) changed - the list
// is a handful: all dropped, all spawned again. The game half's serial window.
void SkeletonEditorWindow::TickAttachments(const SkeletonEditorContext& ctx)
{
    if (!m_attachDirty || !m_stage.Up() || !ctx.world || !ctx.assetMgr) return;
    m_attachDirty = false;
    World& world = *ctx.world;
    for (AttachmentLive& l : m_attachLive) m_stage.Drop(world, ctx.renderer, l.handles);
    m_attachLive.clear();
    for (const PreviewAttachment& a : m_attachments)
    {
        AttachmentLive live;
        live.what = a;
        StageSpawn sp = LoadStageSubject(a.asset, world, *ctx.assetMgr, ctx.renderer, ctx.animClipSys);
        if (sp.root == NullEntity)
        {
            live.error = "it did not load (see the log)";
            LOG_WARNING("[SKELEDITOR] preview attachment '%s' on '%s': %s", a.asset.c_str(), a.socket.c_str(),
                        live.error.c_str());
            m_attachLive.push_back(std::move(live));
            continue;
        }
        const Entity root = sp.root;
        live.handles = m_stage.Adopt(world, std::move(sp));
        live.root    = root;
        world.SetName(root, "[SkeletonEditor] " + a.socket + " <- " + BaseName(a.asset));
        // The follower: at the stage root's socket, by its NAME (FollowSocket re-resolves
        // it whenever the socket list changes - an edit moves it at once). A follower is
        // a root with an identity local transform (it sits ON the socket).
        FollowSocketComponent fs{};
        fs.target.BindAndStamp(world, m_stage.Root());
        fs.socketNameHash  = SocketComponent::HashName(a.socket.c_str());
        fs.socketIndex     = 0;
        fs.cachedSocketGen = 0;
        if (FollowSocketComponent* have = world.GetComponent<FollowSocketComponent>(root)) *have = fs;
        else world.AddComponent<FollowSocketComponent>(root, fs);
        if (LocalTransform* lt = world.GetComponent<LocalTransform>(root)) *lt = LocalTransform{};
        LOG_INFO("[SKELEDITOR] preview attachment '%s' on '%s': root %u, %zu entit%s", a.asset.c_str(),
                 a.socket.c_str(), static_cast<unsigned>(root), live.handles.size(),
                 live.handles.size() == 1 ? "y" : "ies");
        m_attachLive.push_back(std::move(live));
    }
}

// FollowSystem skips an attachment whose socket the stage root does not have,
// so its transform is its own identity local: the world origin, where the
// ray-traced reflections, shadows and GI (and a click) would find it. Park it
// instead at the stage's origin, below the scene, until the socket comes back
// (an undo, a save of its virtual bone).
void SkeletonEditorWindow::ParkAttachments(const SkeletonEditorContext& ctx)
{
    if (m_attachLive.empty() || !m_stage.Up() || !ctx.world) return;
    World& world = *ctx.world;
    const SocketComponent* sc = world.GetComponent<SocketComponent>(m_stage.Root());
    for (AttachmentLive& l : m_attachLive)
    {
        if (l.root == NullEntity || !world.IsAlive(l.root)) continue;
        const bool present =
            sc && sc->FindByName(SocketComponent::HashName(l.what.socket.c_str())) != SocketComponent::kNotFound;
        if (present != l.parked) continue;   // as it should be
        LocalTransform* lt = world.GetComponent<LocalTransform>(l.root);
        if (!lt) continue;
        *lt = LocalTransform{};   // on the socket: the identity offset
        if (!present) lt->translation = XMFLOAT3(0.f, m_stage.OriginY(), 0.f);
        l.parked = !present;
        LOG_INFO("[SKELEDITOR] preview attachment '%s' %s '%s'", l.what.asset.c_str(),
                 l.parked ? "parked below the scene: the stage has no socket" : "back on its socket", l.what.socket.c_str());
    }
}

bool SkeletonEditorWindow::FrameFor(Renderer* renderer, float w, float h, XMFLOAT4X4& toClip, XMFLOAT4X4& toFrame,
                                    const float*& joints, const float*& axes) const
{
    if (!m_loaded || w <= 0.f || h <= 0.f) return false;
    if (StageShown())
    {
        if (!m_live.valid) return false;
        // The stage's projection is for its own size; a picture of another aspect
        // would need its own (the panel always draws the view's own output).
        XMStoreFloat4x4(&toClip, XMMatrixMultiply(XMLoadFloat4x4(&m_live.view), XMLoadFloat4x4(&m_live.proj)));
        toFrame = m_live.rootWorld;
        joints  = m_live.joints.data();
        axes    = m_live.axes.data();
        return true;
    }
    Projection pr;
    if (!MakeProjection(renderer, m_model, m_yaw, m_pitch, m_dist, m_pan, 0.f, 0.f, w, h, pr)) return false;
    XMStoreFloat4x4(&toClip, pr.mvp);
    XMStoreFloat4x4(&toFrame, XMMatrixIdentity());
    joints = m_joints.data();
    axes   = m_axes.data();
    return true;
}

bool SkeletonEditorWindow::SocketFrameMatrix(int index, XMFLOAT4X4& out) const
{
    if (!StageShown()) return SocketModelMatrix(index, out);
    if (!m_live.valid || index < 0 || index >= static_cast<int>(m_sockets.size())) return false;
    const Resource::ModelSocket& s = m_sockets[index];
    if (!s.IsSkeletonSocket()) return false;
    const int b = BindBoneOf(s.bone);   // a virtual bone: its `to` (it IS `to` before IK)
    if (b < 0 || b >= static_cast<int>(m_live.boneWorld.size())) return false;
    const XMFLOAT4X4 off = s.LocalOffset();
    XMStoreFloat4x4(&out, XMMatrixMultiply(XMLoadFloat4x4(&off), XMLoadFloat4x4(&m_live.boneWorld[b])));
    return true;
}

// ==== Draw ====
void SkeletonEditorWindow::Draw(const SkeletonEditorContext& ctx)
{
    ImGui::SetNextWindowSize(ImVec2(1180.f, 680.f), ImGuiCond_FirstUseEver);
    if (m_focusQueued)
    {
        ImGui::SetNextWindowFocus();   // RequestOpen (a double-click, a menu): shown, not left behind a tab
        m_focusQueued = false;
    }
    const std::string title = m_loaded
        ? "Skeleton Editor - " + BaseName(m_iskel) + (Dirty() ? " *" : "") + "###SkeletonEditor"
        : std::string("Skeleton Editor###SkeletonEditor");
    bool keepOpen = true;
    if (!ImGui::Begin(title.c_str(), &keepOpen))
    {
        m_focused = false;
        ImGui::End();
        return;
    }
    // Unsaved edits: the X asks first (Save / Discard / Cancel).
    if (!keepOpen)
    {
        if (Dirty()) { if (m_askFor == 0) { m_askFor = 1; m_askOpenPopup = true; } }
        else m_open = false;
    }
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // A selected socket / virtual bone an undo / a remove took away: the bone is the
    // selection again.
    if (!m_selSocket.empty() && FindSocket(m_selSocket) < 0) m_selSocket.clear();
    if (!m_selVb.empty() && FindVirtualBone(m_selVb) < 0) m_selVb.clear();
    // Ctrl+C / Ctrl+V routed here by EditorLayer (the clipboard is a UI call).
    if (m_copyQueued)
    {
        m_copyQueued = false;
        const int si = SelectedSocket();
        if (si >= 0) ImGui::SetClipboardText(CopySockets({ m_sockets[si].name }).c_str());
    }
    if (m_pasteQueued)
    {
        m_pasteQueued = false;
        const char* clip = ImGui::GetClipboardText();
        std::vector<std::string> added;
        std::string why;
        const std::string toBone = (m_selected >= 0 && m_selected < static_cast<int>(m_names.size()) && m_selSocket.empty())
                                 ? m_names[m_selected] : std::string();
        if (clip && std::strncmp(clip, "SO ", 3) == 0)
        {
            if (PasteSockets(clip, toBone, added, why)) SelectSocket(added.back());
            else m_error = why;
        }
    }
    DrawRenamePopup();
    DrawAskPopup(ctx);

    if (!m_loaded)
    {
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "%s", m_error.c_str());
        ImGui::TextDisabled("Open a skeleton: right-click a skinned model (.iscn) or a skeleton (.iskel) in the\n"
                            "Content Browser > Open in Skeleton Editor, or use the button on a Skeleton\n"
                            "component in the Inspector.");
        ImGui::End();
        return;
    }
    const bool previewOk = ctx.renderer && ctx.renderer->IsPreviewEnabled();
    // The Stage host (Edit mode): ProcessPendingActions spawns the stage and holds
    // its view; the reference host's Synthetic view is requested only when the
    // stage is not what the panel shows (one view at a time).
    const bool stageMode = previewOk && StageMode(ctx.playing);
    if (stageMode)
    {
        m_stageDrawn = true;
        // The panel drew; otherwise the next PPA takes it for hidden and cancels the
        // socket gizmo's mouse drag on the Animation host every frame.
        m_visibleThisFrame = true;
    }
    else if (previewOk) UpdateView(ctx);
    DrawToolbar(ctx);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float statsH = ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
    const float availW = ImGui::GetContentRegionAvail().x;
    const float treeW  = std::clamp(availW * 0.26f, 220.f, 420.f);
    const float rightW = std::clamp(availW * 0.27f, 240.f, 420.f);

    ImGui::BeginChild("##ske_left", ImVec2(treeW, -statsH), true);
    DrawTree();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##ske_view", ImVec2(std::max(64.f, availW - treeW - rightW - 2.f * style.ItemSpacing.x),
                                           -statsH),
                      false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!previewOk)
        ImGui::TextDisabled(ctx.renderer ? "Preview disabled (DX12_PREVIEW=0 or editor_prefs preview.enabled=0)"
                                         : "No renderer attached.");
    else if (m_model.empty())
        ImGui::TextDisabled("No picture: no model beside this .iskel names it (its K line).\n"
                            "The tree and the details still read the skeleton.");
    else
    {
        if (stageMode) DrawTransport();   // the stage's clip above its picture
        DrawViewport(ctx);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##ske_right", ImVec2(0.f, -statsH), true);
    if (ImGui::BeginTabBar("##ske_tabs"))
    {
        // A script / the Inspector's mask preview selects a tab (RequestTab).
        auto flags = [this](Tab t)
        { return m_requestTab == static_cast<int>(t) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
        if (ImGui::BeginTabItem("Details", nullptr, flags(Tab::Details)))
        { m_rightTab = 0; DrawDetails(ctx); ImGui::EndTabItem(); }
        char checkLabel[48];
        std::snprintf(checkLabel, sizeof(checkLabel), "Check (%zu)###ske_check", m_report.issues.size());
        if (ImGui::BeginTabItem(checkLabel, nullptr, flags(Tab::Check))) { m_rightTab = 1; DrawCheck(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Animations", nullptr, flags(Tab::Animations)))
        { m_rightTab = 2; DrawAnimations(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(HumanoidDirty() ? "Retarget *###ske_retarget" : "Retarget###ske_retarget", nullptr,
                                flags(Tab::Retarget)))
        { m_rightTab = 3; DrawRetarget(ctx); ImGui::EndTabItem(); }
        m_requestTab = -1;
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    DrawStatsLine();
    ImGui::End();
}

void SkeletonEditorWindow::DrawToolbar(const SkeletonEditorContext& ctx)
{
    if (ImGui::Button("Browse") && ctx.reveal) ctx.reveal(m_model.empty() ? m_iskel : m_model);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show it in the Content Browser");
    ImGui::SameLine();
    // The host: the bind pose in the studio, or the live pose on a stage.
    int hi = m_host == Host::Stage ? 1 : 0;
    ImGui::SetNextItemWidth(170.f);
    ImGui::BeginDisabled(m_model.empty());
    if (ImGui::BeginCombo("##ske_host", kHosts[hi].label))
    {
        for (int i = 0; i < static_cast<int>(std::size(kHosts)); ++i)
            if (ImGui::Selectable(kHosts[i].label, i == hi))
            {
                if (kHosts[i].host == Host::Stage && m_stageRefusedFor == m_model) m_stageRefusedFor.clear();   // try again
                m_host = kHosts[i].host;
            }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", m_model.empty()
                                    ? "No model: the picture needs one."
                                : ctx.playing
                                    ? "Playing: the stage is down until Stop - the reference pose meanwhile."
                                    : "Reference Pose: the model at its bind pose in the studio.\n"
                                      "Animation: the model spawned on a hidden stage of the scene and seen\n"
                                      "live - the pose the engine gives it (its chains stay at rest here).");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    int si = 0;
    for (int i = 0; i < static_cast<int>(std::size(kScopes)); ++i)
        if (kScopes[i].scope == m_scope) si = i;
    ImGui::SetNextItemWidth(210.f);
    if (ImGui::BeginCombo("##ske_scope", kScopes[si].label))
    {
        for (int i = 0; i < static_cast<int>(std::size(kScopes)); ++i)
            if (ImGui::Selectable(kScopes[i].label, i == si)) m_scope = kScopes[i].scope;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which bones the picture draws");
    ImGui::SameLine();
    if (ImGui::Button("Show")) ImGui::OpenPopup("##ske_show");
    if (ImGui::BeginPopup("##ske_show"))
    {
        for (const ShowEntry& e : kShows)
        {
            bool on = (m_show & e.bit) != 0;
            if (ImGui::Checkbox(e.label, &on)) m_show = on ? (m_show | e.bit) : (m_show & ~e.bit);
        }
        ImGui::Separator();
        ImGui::BeginDisabled(StageShown());   // the studio's options (the stage is the scene's)
        ImGui::Checkbox("Mesh", &m_meshVisible);
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool stage = StageShown();
    ImGui::BeginDisabled(stage);
    static const char* kBgNames[] = { "Solid", "Checker", "Skybox" };
    ImGui::SetNextItemWidth(90.f);
    ImGui::Combo("##ske_bg", &m_background, kBgNames, IM_ARRAYSIZE(kBgNames));
    ImGui::SameLine();
    ImGui::Checkbox("Shadow", &m_shadow);
    ImGui::SameLine();
    // The weight heatmap (the selected bone's weights on the skinned parts): the
    // reference host's (it colours the studio's copies of the parts).
    ImGui::BeginDisabled(m_weights.Parts() == 0);
    ImGui::Checkbox("Weights", &m_weightsOn);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(stage ? "The reference host's: the stage shows the scene's own rendering."
                          : m_weights.Parts() == 0
                              ? "No skinned part pairs with the skeleton's weights here."
                              : "Colour the skinned parts by the selected bone's weights:\n"
                                "dark blue = not at all, then cyan, green, yellow, red = only it.");
    ImGui::SameLine();
    if (ImGui::Button("Reset view")) ResetView();

    // The sockets' and virtual bones' Save / Revert / history, and the gizmo.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const bool dirty = Dirty();
    const std::string lock = SocketLock();
    // An .iskel opened alone saves its humanoid mapping (it has no records).
    ImGui::BeginDisabled(!dirty || (!lock.empty() && !HumanoidDirty()) || ctx.playing);
    if (ImGui::Button("Save")) m_saveQueued = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(ctx.playing ? "Stop first: a saved model re-derives its sockets into the world."
                                      : "Write the model's socket and virtual bone records, and a changed humanoid\n"
                                        "mapping into the .iskel. Every placed instance follows the sockets and the\n"
                                        "mapping at once, the virtual bones when it spawns again.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Revert")) Revert();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::ArrowButton("##ske_undo", ImGuiDir_Left)) RequestUndo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Undo (Ctrl+Z)");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_redo.empty());
    if (ImGui::ArrowButton("##ske_redo", ImGuiDir_Right)) RequestRedo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Redo (Ctrl+Y)");
    ImGui::SameLine();
    static const char* kOps[] = { "Off", "Move (W)", "Rotate (E)", "Scale (R)" };
    int op = static_cast<int>(m_gizmoOp);
    ImGui::SetNextItemWidth(100.f);
    if (ImGui::Combo("##ske_gizmo", &op, kOps, IM_ARRAYSIZE(kOps))) SetGizmoOp(static_cast<GizmoOp>(op));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The selected socket's gizmo");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_iskel.c_str());
    if (!m_lastSave.empty()) ImGui::TextDisabled("%s", m_lastSave.c_str());
    // Placed instances whose skeleton lacks the saved virtual bones: they take
    // them when they spawn again.
    if (m_staleInstances > 0)
    {
        ImGui::TextColored(kColVirtualText, "%u placed instance(s) of this model have older virtual bones: they take the "
                           "saved ones when the scene loads again.", m_staleInstances);
        ImGui::SameLine();
        ImGui::BeginDisabled(!ctx.reloadScene || ctx.playing);
        if (ImGui::SmallButton("Reload Scene") && ctx.reloadScene && !ctx.reloadScene())
            m_error = "The scene has no file to reload - save it first (or place the model again).";
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Load the scene file again (it asks about unsaved scene changes first).");
    }
    // Why the Animation host shows the reference pose.
    if (m_host == Host::Stage && !m_model.empty())
    {
        if (ctx.playing)
            ImGui::TextDisabled("Animation host: the stage is down while playing - the reference pose until Stop.");
        else if (!m_stageStatus.empty())
            ImGui::TextColored(ImVec4(1.f, 0.72f, 0.3f, 1.f), "%s", m_stageStatus.c_str());
    }
    if (!m_error.empty()) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.4f, 1.f), "%s", m_error.c_str());
}

void SkeletonEditorWindow::DrawViewport(const SkeletonEditorContext& ctx)
{
    Renderer* renderer = ctx.renderer;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool   stageMode = StageMode(ctx.playing);
    if (m_harnessW != 0)
    {
        m_renderW = m_pendW = m_harnessW;
        m_renderH = m_pendH = m_harnessH;
    }
    else
    {
        std::uint32_t w = 0, h = 0;
        PreviewPanel::QuantiseBox(avail.x, avail.y, w, h);
        const double now = ImGui::GetTime();
        // The FIRST measurement of a showing is the size at once: nothing was drawn at
        // another (UpdateView waited for it). Only a change after it - a drag, a line
        // of the toolbar coming or going - waits out the debounce (a recompile
        // re-creates the view's textures).
        if (m_renderW == 0 || m_renderH == 0)
        {
            m_renderW = m_pendW = w;
            m_renderH = m_pendH = h;
            m_pendAt  = now;
        }
        else if (w != m_pendW || h != m_pendH) { m_pendW = w; m_pendH = h; m_pendAt = now; }
        else if ((m_pendW != m_renderW || m_pendH != m_renderH) && now - m_pendAt >= kResizeDebounceSeconds)
        {
            m_renderW = m_pendW;
            m_renderH = m_pendH;
        }
    }
    // The stage's view when the Animation host shows it, else the reference host's.
    const std::uint32_t view = stageMode ? (StageShown() ? m_stage.View() : ~0u) : m_viewId;
    const std::uint64_t srv = (view != ~0u) ? renderer->GetPreviewSrv(view) : 0;
    std::uint32_t ow = 0, oh = 0;
    if (view != ~0u) renderer->GetPreviewOutputSize(view, ow, oh);
    if (!srv || ow == 0 || oh == 0)
    {
        ImGui::Dummy(ImVec2(std::max(1.f, avail.x), std::max(1.f, avail.y)));
        ImGui::SetCursorPos(ImVec2(12.f, 12.f));
        if (stageMode && !m_stage.Up())
            ImGui::TextDisabled("Spawning the stage...");
        else
            ImGui::TextDisabled(view == ~0u ? "No preview view is free (camera previews, split views and the other\n"
                                              "preview panels hold them all) - close one to see the model."
                                            : "Rendering...");
        return;
    }
    float s = std::min(avail.x / static_cast<float>(ow), avail.y / static_cast<float>(oh));
    if (!(s > 0.f)) s = 1.f;
    const ImVec2 size(static_cast<float>(ow) * s, static_cast<float>(oh) * s);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 origin(start.x + std::max(0.f, (avail.x - size.x) * 0.5f),
                        start.y + std::max(0.f, (avail.y - size.y) * 0.5f));
    ImGui::SetCursorScreenPos(origin);
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)), size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    m_lastOverlay = DrawOverlay(renderer, dl, origin.x, origin.y, size.x, size.y);
    char badge[128];
    std::snprintf(badge, sizeof(badge), "%s%s  |  %u bone(s) drawn", StageShown() ? "LIVE stage  |  " : "",
                  kScopes[static_cast<int>(m_scope)].label, m_lastOverlay.bones);
    dl->AddText(ImVec2(origin.x + 8.f, origin.y + 6.f), IM_COL32(230, 230, 230, 220), badge);
    // Picking a virtual bone's target in the picture.
    if (m_pickTargetFrom >= 0 && m_pickTargetFrom < static_cast<int>(m_names.size()))
    {
        char pick[160];
        std::snprintf(pick, sizeof(pick), "Click the bone the virtual bone from '%s' follows  (Esc: cancel)",
                      m_names[m_pickTargetFrom].c_str());
        const ImVec2 at(origin.x + 8.f, origin.y + 24.f);
        const ImVec2 sz = ImGui::CalcTextSize(pick);
        dl->AddRectFilled(ImVec2(at.x - 4.f, at.y - 2.f), ImVec2(at.x + sz.x + 4.f, at.y + sz.y + 2.f),
                          IM_COL32(40, 10, 60, 210), 3.f);
        dl->AddText(at, kColVirtual, pick);
    }
    dl->PopClipRect();

    // ---- The socket gizmo ----
    // Before ##ske_orbit: ImGuizmo's patched CanActivate needs HoveredId == 0 &&
    // ActiveId == 0 on the click frame.
    DrawSocketGizmo(renderer, origin.x, origin.y, size.x, size.y);
    // Orbit / pan stand aside while the gizmo is hovered or held (a click that
    // grabs a handle must not also orbit); a hover while an orbit runs does not.
    const bool gizmoBusy = m_gizmoUsing || (m_gizmoOver && !m_orbitActive);

    // ---- Orbit / pan / zoom on the panel's own state; a click picks a joint ----
    ImGui::SetCursorScreenPos(origin);
    if (gizmoBusy) ImGui::BeginDisabled();
    ImGui::InvisibleButton("##ske_orbit", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    if (gizmoBusy) ImGui::EndDisabled();
    m_orbitActive = ImGui::IsItemActive();
    if (ImGui::IsItemActive())
    {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            m_yaw  += d.x * 0.01f;
            m_pitch = std::clamp(m_pitch - d.y * 0.01f, -1.4f, 1.4f);
        }
        else if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            const Preview::StudioCamera& sc = Preview::kStudioCamera;
            const float dist = std::max(0.35f, sc.distance * std::max(0.05f, m_dist));
            const float wpp  = 2.f * dist * std::tan(sc.fovY * 0.5f) / std::max(1.f, size.y);
            const PreviewPanel::Cam cam =
                PreviewPanel::BuildCam(m_yaw, m_pitch, m_dist, m_pan, size.x / std::max(1.f, size.y));
            XMFLOAT3 r, u;
            XMStoreFloat3(&r, cam.right);
            XMStoreFloat3(&u, cam.up);
            m_pan[0] += (-d.x * r.x + d.y * u.x) * wpp;
            m_pan[1] += (-d.x * r.y + d.y * u.y) * wpp;
            m_pan[2] += (-d.x * r.z + d.y * u.z) * wpp;
        }
    }
    // A click (released without a drag) picks the nearest joint or socket (a tie:
    // the bone).
    if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
        if (drag.x * drag.x + drag.y * drag.y < 9.f)
        {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float px = (m.x - origin.x) / s, py = (m.y - origin.y) / s;
            if (m_pickTargetFrom >= 0)
            {
                // The target of a virtual bone: any joint, drawn or not.
                const int from = m_pickTargetFrom;
                m_pickTargetFrom = -1;
                const int to = PickAt(renderer, px, py, /*anyJoint=*/true);
                std::string name, why;
                if (to < 0) m_error = "No joint there - Add Virtual Bone cancelled.";
                else if (from < static_cast<int>(m_names.size()) &&
                         AddVirtualBone(m_names[from], m_names[to], "", name, why))
                    SelectVirtualBone(name);
                else m_error = why;
            }
            else
            {
                const int hit  = PickAt(renderer, px, py);
                const int sock = PickSocketAt(renderer, px, py);
                float bx = 0.f, by = 0.f, sx = 0.f, sy = 0.f;
                const bool boneAt = hit >= 0 && ProjectJoint(renderer, hit, bx, by);
                const bool sockAt = sock >= 0 && ProjectSocket(renderer, sock, sx, sy);
                const float bd = boneAt ? (bx - px) * (bx - px) + (by - py) * (by - py) : FLT_MAX;
                const float sd = sockAt ? (sx - px) * (sx - px) + (sy - py) * (sy - py) : FLT_MAX;
                if (sockAt && sd < bd) SelectSocket(m_sockets[sock].name);
                else if (hit >= 0)     Select(hit);
            }
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) m_dist = std::clamp(m_dist * (1.f - wheel * 0.1f), 0.05f, 8.f);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) ResetView();
        if (m_pickTargetFrom >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_pickTargetFrom = -1;
        // The gizmo's operation - never mid-drag (switching it while ImGuizmo is
        // using strands its mbUsing: the release becomes unreachable).
        if (!m_gizmoUsing && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) SetGizmoOp(GizmoOp::Translate);
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) SetGizmoOp(GizmoOp::Rotate);
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) SetGizmoOp(GizmoOp::Scale);
        }
        if (ImGui::GetIO().KeyShift)
            ImGui::SetTooltip("Left-drag orbit, right/middle-drag pan, wheel zoom, F frame.\n"
                              "Click a joint or a socket to select it; W / E / R: move / rotate / scale the\n"
                              "selected socket. The model is drawn at its bind pose.");
    }
}

// ==== The overlay ====
void SkeletonEditorWindow::DrawnBones(std::vector<std::uint8_t>& out) const
{
    const int n = static_cast<int>(m_names.size());
    out.assign(n, 0);
    if (m_scope == BoneScope::None) return;
    if (m_scope == BoneScope::All) { std::fill(out.begin(), out.end(), 1); return; }
    if (m_selected < 0 || m_selected >= n) return;
    out[m_selected] = 1;
    if (m_scope == BoneScope::SelectedAndParents || m_scope == BoneScope::Family)
        for (int a = m_report.bones[m_selected].parent, guard = 0; a >= 0 && a < n && guard < n;
             a = m_report.bones[a].parent, ++guard)
            out[a] = 1;
    if (m_scope == BoneScope::SelectedAndChildren || m_scope == BoneScope::Family)
    {
        std::vector<int> stack(m_children[m_selected].begin(), m_children[m_selected].end());
        while (!stack.empty())
        {
            const int c = stack.back();
            stack.pop_back();
            if (out[c]) continue;
            out[c] = 1;
            stack.insert(stack.end(), m_children[c].begin(), m_children[c].end());
        }
    }
}

SkeletonEditorWindow::OverlayStats SkeletonEditorWindow::DrawOverlay(Renderer* renderer, ImDrawList* dl, float x,
                                                                     float y, float w, float h) const
{
    using namespace Resource;
    OverlayStats st;
    if (!m_loaded || m_harnessOverlayOff) return st;
    // The host drawn now: the bind pose in model space (reference) or the live
    // pose in world space (stage).
    XMFLOAT4X4 toClip, toFrame;
    const float* joints  = nullptr;
    const float* axesSrc = nullptr;
    if (!FrameFor(renderer, w, h, toClip, toFrame, joints, axesSrc)) return st;
    Projection pr;
    pr.mvp = XMLoadFloat4x4(&toClip);
    pr.x = x;
    pr.y = y;
    pr.w = w;
    pr.h = h;
    const XMMATRIX frame = XMLoadFloat4x4(&toFrame);
    const int n = static_cast<int>(m_names.size());
    std::vector<std::uint8_t> drawn;
    DrawnBones(drawn);

    auto J = [&](int b) { return joints + std::size_t(b) * 3; };
    auto line3 = [&](const float* a, const float* b, ImU32 col, float thick)
    {
        if (st.lines >= kMaxOverlayLines) return;
        ImVec2 pa, pb;
        if (!pr.Project(a, pa) || !pr.Project(b, pb)) return;
        dl->AddLine(pa, pb, col, thick);
        ++st.lines;
    };

    // Ground grid at the joints' lowest point (model space, into the frame's).
    if (m_show & kShowGrid)
    {
        const float cx = 0.5f * (m_boundsMin[0] + m_boundsMax[0]), cz = 0.5f * (m_boundsMin[2] + m_boundsMax[2]);
        const float half = 0.6f * std::max({ m_boundsMax[0] - m_boundsMin[0], m_boundsMax[2] - m_boundsMin[2],
                                             0.5f * m_modelSize });
        auto toF = [&](float px, float py, float pz, float* out)
        {
            XMFLOAT3 q;
            XMStoreFloat3(&q, XMVector3TransformCoord(XMVectorSet(px, py, pz, 1.f), frame));
            out[0] = q.x; out[1] = q.y; out[2] = q.z;
        };
        const int k = 10;
        for (int i = 0; i <= k; ++i)
        {
            const float t = -half + 2.f * half * static_cast<float>(i) / k;
            float a[3], b[3], c[3], d[3];
            toF(cx + t, m_boundsMin[1], cz - half, a);
            toF(cx + t, m_boundsMin[1], cz + half, b);
            toF(cx - half, m_boundsMin[1], cz + t, c);
            toF(cx + half, m_boundsMin[1], cz + t, d);
            const ImU32 col = (i == k / 2) ? IM_COL32(150, 150, 150, 150) : IM_COL32(110, 110, 110, 90);
            line3(a, b, col, 1.f);
            line3(c, d, col, 1.f);
        }
    }

    // The reference pose's ghost under the live pose (the stage host): the bind
    // skeleton, faint, placed where the stage is (model space -> the stage's
    // world).
    if ((m_show & kShowGhost) && StageShown())
    {
        auto bindAt = [&](int b, float* out)
        {
            XMFLOAT3 q;
            XMStoreFloat3(&q, XMVector3TransformCoord(
                XMVectorSet(m_joints[std::size_t(b) * 3], m_joints[std::size_t(b) * 3 + 1],
                            m_joints[std::size_t(b) * 3 + 2], 1.f), frame));
            out[0] = q.x; out[1] = q.y; out[2] = q.z;
        };
        for (int b = 0; b < n; ++b)
        {
            const int p = m_report.bones[b].parent;
            if (!drawn[b] || p < 0 || p >= n || p == b) continue;
            float pa[3], pb[3];
            bindAt(p, pa);
            bindAt(b, pb);
            const std::uint32_t before = st.lines;
            line3(pa, pb, IM_COL32(190, 200, 255, 70), 1.f);
            st.ghost += st.lines != before ? 1u : 0u;
        }
    }

    // The selection's ancestors / descendants, for colouring.
    std::vector<std::uint8_t> rel(n, 0);   // 1 ancestor, 2 descendant
    if (m_selected >= 0 && m_selected < n)
    {
        for (int a = m_report.bones[m_selected].parent, guard = 0; a >= 0 && a < n && guard < n;
             a = m_report.bones[a].parent, ++guard)
            rel[a] = 1;
        std::vector<int> stack(m_children[m_selected].begin(), m_children[m_selected].end());
        while (!stack.empty())
        {
            const int c = stack.back();
            stack.pop_back();
            if (rel[c] == 2) continue;
            rel[c] = 2;
            stack.insert(stack.end(), m_children[c].begin(), m_children[c].end());
        }
    }
    auto colourOf = [&](int b) -> ImU32
    {
        if (b == m_selected) return kColSelected;
        // The bone mask preview is the question being asked: it wins over categories.
        if (m_maskMode >= 0) return InMask(b) ? kColMaskIn : kColMaskOut;
        if (m_show & kShowCategories)
        {
            const std::uint32_t f = m_report.bones[b].flags;
            if (f & kSkelBoneIKBone) return kColIKBone;
            if (f & kSkelBoneIKTarget) return kColIKTarget;
            if (f & kSkelBoneIKLink) return kColIKLink;
            if (f & kSkelBoneGrantTarget) return kColGrant;
            if (f & kSkelBoneTwist) return kColTwist;
            if (f & kSkelBoneChainName) return kColChain;
            if (f & kSkelBoneDummyLeaf) return kColDummy;
        }
        if (rel[b] == 1) return kColAncestor;
        if (rel[b] == 2) return kColDescendant;
        return kColBone;
    };

    // Bones: parent joint -> this joint, as a line or an octahedron (UE's shape).
    for (int b = 0; b < n; ++b)
    {
        if (!drawn[b]) continue;
        ++st.bones;
        st.masked += InMask(b) ? 1u : 0u;
        const int p = m_report.bones[b].parent;
        if (p < 0 || p >= n || p == b) continue;
        const ImU32 col   = colourOf(b);
        const float thick = b == m_selected ? 2.5f : 1.5f;
        const float* base = J(p);
        const float* tip  = J(b);
        const XMVECTOR B = XMVectorSet(base[0], base[1], base[2], 1.f);
        const XMVECTOR T = XMVectorSet(tip[0], tip[1], tip[2], 1.f);
        const XMVECTOR D = XMVectorSubtract(T, B);
        const float len = XMVectorGetX(XMVector3Length(D));
        if (!(m_show & kShowOctahedra) || len < 1e-4f * m_modelSize)
        {
            line3(base, tip, col, thick);
            continue;
        }
        const XMVECTOR dn = XMVectorScale(D, 1.f / len);
        const XMVECTOR helper = std::fabs(XMVectorGetY(dn)) < 0.9f ? XMVectorSet(0.f, 1.f, 0.f, 0.f)
                                                                    : XMVectorSet(1.f, 0.f, 0.f, 0.f);
        const XMVECTOR u = XMVector3Normalize(XMVector3Cross(dn, helper));
        const XMVECTOR v = XMVector3Cross(dn, u);
        const float rad = std::clamp(len * 0.1f, 0.002f * m_modelSize, 0.03f * m_modelSize);
        const XMVECTOR ring = XMVectorAdd(B, XMVectorScale(D, 0.15f));
        float r[4][3];
        const XMVECTOR off[4] = { XMVectorScale(u, rad), XMVectorScale(v, rad), XMVectorScale(u, -rad),
                                  XMVectorScale(v, -rad) };
        for (int i = 0; i < 4; ++i)
        {
            XMFLOAT3 q;
            XMStoreFloat3(&q, XMVectorAdd(ring, off[i]));
            r[i][0] = q.x; r[i][1] = q.y; r[i][2] = q.z;
        }
        for (int i = 0; i < 4; ++i)
        {
            line3(base, r[i], col, thick * 0.8f);
            line3(r[i], tip, col, thick * 0.8f);
            line3(r[i], r[(i + 1) % 4], col, thick * 0.8f);
        }
    }

    // Joints.
    if (m_show & kShowJoints)
        for (int b = 0; b < n; ++b)
        {
            if (!drawn[b]) continue;
            ImVec2 q;
            if (!pr.Project(J(b), q)) continue;
            const bool sel = b == m_selected;
            dl->AddCircleFilled(q, sel ? 4.5f : 2.5f, sel ? kColSelected : colourOf(b), 10);
            ++st.joints;
        }

    // The humanoid roles (Unity's Avatar): a required role's joint a filled
    // circle, an optional one's a dashed ring - in the Retarget tab, or with the
    // show bit.
    if (((m_show & kShowHumanoid) || m_rightTab == static_cast<int>(Tab::Retarget)) && m_skel)
    {
        const HumanoidProfile& hp = m_skel->humanoid;
        for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r)
        {
            const int b = hp.boneForRole[r];
            if (b < 0 || b >= n || !drawn[b]) continue;
            ImVec2 q;
            if (!pr.Project(J(b), q)) continue;
            constexpr float kR = 6.5f;
            if (IsRequiredHumanoidBone(static_cast<HumanoidBone>(r)))
            {
                dl->AddCircle(q, kR, kColRole, 16, 2.f);
                dl->AddCircleFilled(q, kR - 2.5f, kColRole, 12);
            }
            else
            {
                constexpr int kDashes = 8;
                for (int k = 0; k < kDashes; ++k)
                {
                    const float a0 = 2.f * 3.14159265f * k / kDashes, a1 = a0 + 3.14159265f / kDashes;
                    dl->PathArcTo(q, kR, a0, a1, 4);
                    dl->PathStroke(kColRole, 0, 1.5f);
                }
            }
            ++st.humanoid;
        }
    }

    // Local axes (the bind / live rotation's rows), X red, Y green, Z blue.
    auto axes = [&](int b)
    {
        const float lenBone = m_report.bones[b].length;
        const float L = std::max(0.02f * m_modelSize, 0.35f * lenBone);
        const float* o = J(b);
        const ImU32 cols[3] = { IM_COL32(240, 70, 70, 255), IM_COL32(80, 220, 80, 255), IM_COL32(80, 130, 255, 255) };
        for (int a = 0; a < 3; ++a)
        {
            const float* ax = axesSrc + std::size_t(b) * 9 + a * 3;
            const float e[3] = { o[0] + ax[0] * L, o[1] + ax[1] * L, o[2] + ax[2] * L };
            line3(o, e, cols[a], 1.5f);
        }
    };
    if (m_show & kShowAllAxes)
    {
        for (int b = 0; b < n; ++b)
            if (drawn[b]) axes(b);
    }
    else if ((m_show & kShowAxes) && m_selected >= 0 && m_selected < n && m_selSocket.empty() && m_selVb.empty())
        axes(m_selected);

    // The virtual bones: a dashed line from `from`'s joint to `to`'s and a hollow
    // diamond at `to` (where the virtual bone is at the bind pose), drawn with
    // either of its bones (or when selected); the selected one in the selection
    // colour, named.
    if (m_show & kShowVirtual)
    {
        const int selVb = SelectedVirtualBone();
        for (int v = 0; v < static_cast<int>(m_vbs.size()) && v < static_cast<int>(m_vbProblems.size()); ++v)
        {
            if (!m_vbProblems[v].empty()) continue;
            const int f = m_vbFrom[v], t = m_vbTo[v];
            if (f < 0 || f >= n || t < 0 || t >= n) continue;
            const bool isSel = v == selVb;
            if (!isSel && !drawn[f] && !drawn[t]) continue;
            ImVec2 a, b;
            if (!pr.Project(J(f), a) || !pr.Project(J(t), b)) continue;
            const ImU32 col   = isSel ? kColSelected : kColVirtual;
            const float thick = isSel ? 2.f : 1.5f;
            constexpr int kDashes = 14;   // segments; every other one drawn
            for (int k = 0; k < kDashes && st.lines < kMaxOverlayLines; k += 2)
            {
                const float t0 = static_cast<float>(k) / kDashes, t1 = static_cast<float>(k + 1) / kDashes;
                dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0),
                            ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1), col, thick);
                ++st.lines;
            }
            const float r = isSel ? 8.f : 6.f;
            dl->AddQuad(ImVec2(b.x, b.y - r), ImVec2(b.x + r, b.y), ImVec2(b.x, b.y + r), ImVec2(b.x - r, b.y), col,
                        thick);
            ++st.virtualBones;
            if (isSel && (m_show & (kShowNames | kShowAllNames)))
            {
                const char* text = m_vbs[v].name.c_str();
                const ImVec2 at(b.x + 10.f, b.y + 6.f);
                const ImVec2 sz = ImGui::CalcTextSize(text);
                dl->AddRectFilled(ImVec2(at.x - 3.f, at.y - 1.f), ImVec2(at.x + sz.x + 3.f, at.y + sz.y + 1.f),
                                  IM_COL32(0, 0, 0, 170), 3.f);
                dl->AddText(at, kColVirtual, text);
                ++st.labels;
            }
        }
    }

    // The sockets: a diamond each; the selected one larger, with its axes, a line
    // to its bone's joint and its name.
    if (m_show & kShowSockets)
    {
        const int sel = SelectedSocket();
        for (int i = 0; i < static_cast<int>(m_sockets.size()); ++i)
        {
            XMFLOAT4X4 m;
            if (!SocketFrameMatrix(i, m)) continue;
            const float p[3] = { m._41, m._42, m._43 };
            ImVec2 q;
            if (!pr.Project(p, q)) continue;
            const bool isSel = i == sel;
            const float r = isSel ? 6.5f : 4.5f;
            const ImU32 col = isSel ? kColSelected : kColSocket;
            dl->AddQuadFilled(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), col);
            dl->AddQuad(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y),
                        IM_COL32(0, 0, 0, 200), 1.f);
            ++st.sockets;
            if (!isSel) continue;
            const int b = BindBoneOf(m_sockets[i].bone);
            if (b >= 0) line3(J(b), p, IM_COL32(200, 200, 200, 150), 1.f);
            const float L = 0.05f * m_modelSize;
            const ImU32 cols[3] = { IM_COL32(240, 70, 70, 255), IM_COL32(80, 220, 80, 255), IM_COL32(80, 130, 255, 255) };
            const float rows[3][3] = { { m._11, m._12, m._13 }, { m._21, m._22, m._23 }, { m._31, m._32, m._33 } };
            for (int a = 0; a < 3; ++a)
            {
                const float len = std::sqrt(rows[a][0] * rows[a][0] + rows[a][1] * rows[a][1] + rows[a][2] * rows[a][2]);
                if (!(len > 1e-8f)) continue;
                const float e[3] = { p[0] + rows[a][0] / len * L, p[1] + rows[a][1] / len * L, p[2] + rows[a][2] / len * L };
                line3(p, e, cols[a], 2.f);
            }
            if (m_show & (kShowNames | kShowAllNames))
            {
                const char* text = m_sockets[i].name.c_str();
                const ImVec2 at(q.x + 8.f, q.y + 4.f);
                const ImVec2 sz = ImGui::CalcTextSize(text);
                dl->AddRectFilled(ImVec2(at.x - 3.f, at.y - 1.f), ImVec2(at.x + sz.x + 3.f, at.y + sz.y + 1.f),
                                  IM_COL32(0, 0, 0, 170), 3.f);
                dl->AddText(at, kColSocket, text);
                ++st.labels;
            }
        }
    }

    // Names: the selected one boxed; every drawn one decluttered on a grid.
    auto label = [&](int b, bool boxed)
    {
        ImVec2 q;
        if (!pr.Project(J(b), q)) return false;
        const char* text = m_names[b].empty() ? "(unnamed)" : m_names[b].c_str();
        const ImVec2 at(q.x + 6.f, q.y - 16.f);
        if (boxed)
        {
            const ImVec2 sz = ImGui::CalcTextSize(text);
            dl->AddRectFilled(ImVec2(at.x - 3.f, at.y - 1.f), ImVec2(at.x + sz.x + 3.f, at.y + sz.y + 1.f),
                              IM_COL32(0, 0, 0, 170), 3.f);
        }
        dl->AddText(at, boxed ? kColSelected : IM_COL32(225, 225, 235, 210), text);
        ++st.labels;
        return true;
    };
    if (m_show & kShowAllNames)
    {
        // One label per 90 x 14 px cell: a thousand names do not become one blot.
        std::vector<std::uint8_t> taken;
        const int cw = 90, chh = 14;
        const int gx = std::max(1, static_cast<int>(w) / cw + 1), gy = std::max(1, static_cast<int>(h) / chh + 1);
        taken.assign(std::size_t(gx) * gy, 0);
        for (int b = 0; b < n; ++b)
        {
            if (!drawn[b] || b == m_selected) continue;
            ImVec2 q;
            if (!pr.Project(J(b), q)) continue;
            const int cx = static_cast<int>((q.x - x) / cw), cy = static_cast<int>((q.y - y) / chh);
            if (cx < 0 || cy < 0 || cx >= gx || cy >= gy) continue;
            std::uint8_t& t = taken[std::size_t(cy) * gx + cx];
            if (t) continue;
            t = 1;
            label(b, false);
        }
    }
    if ((m_show & (kShowNames | kShowAllNames)) && m_selected >= 0 && m_selected < n) label(m_selected, true);
    return st;
}

int SkeletonEditorWindow::PickAt(Renderer* renderer, float px, float py, bool anyJoint) const
{
    const std::uint32_t view = CurrentView();   // the host drawn now
    if (!m_loaded || view == ~0u || !renderer) return -1;
    std::uint32_t ow = 0, oh = 0;
    if (!renderer->GetPreviewOutputSize(view, ow, oh) || !ow || !oh) return -1;
    XMFLOAT4X4 toClip, toFrame;
    const float* joints = nullptr;
    const float* axes   = nullptr;
    if (!FrameFor(renderer, static_cast<float>(ow), static_cast<float>(oh), toClip, toFrame, joints, axes)) return -1;
    const Projection pr = ProjectionOf(toClip, 0.f, 0.f, static_cast<float>(ow), static_cast<float>(oh));
    std::vector<std::uint8_t> drawn;
    DrawnBones(drawn);
    const bool any = !anyJoint && std::find(drawn.begin(), drawn.end(), std::uint8_t{ 1 }) != drawn.end();
    int best = -1;
    float bestD2 = kPickRadiusPx * kPickRadiusPx;
    for (int b = 0; b < static_cast<int>(m_names.size()); ++b)
    {
        if (any && !drawn[b]) continue;
        ImVec2 q;
        if (!pr.Project(joints + std::size_t(b) * 3, q)) continue;
        const float dx = q.x - px, dy = q.y - py, d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = b; }
    }
    return best;
}

bool SkeletonEditorWindow::ProjectJoint(Renderer* renderer, int bone, float& px, float& py) const
{
    const std::uint32_t view = CurrentView();
    if (!m_loaded || view == ~0u || !renderer || bone < 0 || bone >= static_cast<int>(m_names.size()))
        return false;
    std::uint32_t ow = 0, oh = 0;
    if (!renderer->GetPreviewOutputSize(view, ow, oh) || !ow || !oh) return false;
    XMFLOAT4X4 toClip, toFrame;
    const float* joints = nullptr;
    const float* axes   = nullptr;
    if (!FrameFor(renderer, static_cast<float>(ow), static_cast<float>(oh), toClip, toFrame, joints, axes)) return false;
    const Projection pr = ProjectionOf(toClip, 0.f, 0.f, static_cast<float>(ow), static_cast<float>(oh));
    ImVec2 q;
    if (!pr.Project(joints + std::size_t(bone) * 3, q)) return false;
    px = q.x;
    py = q.y;
    return true;
}

long long SkeletonEditorWindow::WriteOverlayPng(Renderer* renderer, const std::string& path, std::string& why)
{
    const std::uint32_t view = CurrentView();   // the host drawn now
    if (!m_loaded || !renderer || view == ~0u) { why = "no view"; return -1; }
    if (!ImGui::GetCurrentContext()) { why = "no ImGui context"; return -1; }
    std::uint32_t w = 0, h = 0;
    if (!renderer->GetPreviewOutputSize(view, w, h) || !w || !h) { why = "no picture yet"; return -1; }
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
    dl.PushClipRect(ImVec2(0.f, 0.f), ImVec2(static_cast<float>(w), static_cast<float>(h)), false);
    // Labels come out as solid quads (DrawListRaster ignores textures), like the
    // chain overlay's text.
    const OverlayStats st = DrawOverlay(renderer, &dl, 0.f, 0.f, static_cast<float>(w), static_cast<float>(h));
    std::uint32_t tris = 0;
    const long long lit = RasterizeDrawListToPng(dl, w, h, path, &tris, &why);
    if (lit >= 0)
        LOG_INFO("[SKELEDITOR] overlay shot %s: file='%s' lit=%lld bones=%u joints=%u lines=%u labels=%u scope=%s "
                 "show=%s selected=%d size=%ux%u", BaseName(path).c_str(), path.c_str(), lit, st.bones, st.joints,
                 st.lines, st.labels, BoneScopeName(m_scope), ShowNames(m_show).c_str(), m_selected, w, h);
    return lit;
}

// ==== State ====
SkeletonEditorWindow::StateView SkeletonEditorWindow::State(Renderer* renderer) const
{
    StateView s;
    s.open    = m_open;
    s.loaded  = m_loaded;
    s.preview = !m_model.empty();
    s.path    = m_path;
    s.model   = m_model;
    s.iskel   = m_iskel;
    s.error   = m_error;
    s.humanoidSource = m_humanoidSource;
    s.viewId  = CurrentView();   // the host drawn now (the stage's, or the reference host's)
    s.renderW  = m_renderW;
    s.renderH  = m_renderH;
    s.pendingW = m_pendW;
    s.pendingH = m_pendH;
    if (renderer && s.viewId != ~0u)
    {
        renderer->GetPreviewOutputSize(s.viewId, s.outputW, s.outputH);
        s.renders = renderer->GetPreviewViewRenderCount(s.viewId);
        s.lease   = renderer->GetPreviewViewLease(s.viewId);
    }
    s.meshVisible = m_meshVisible;
    // The stage host
    s.host             = HostName(m_host);
    s.stageShown       = StageShown();
    s.stageUp          = m_stage.Up();
    s.stagePinned      = m_stage.Pinned();
    s.stageSharingLane = m_stage.SharingLane();
    s.stageRoot        = m_stage.Root();
    s.stageSkeleton    = m_stage.Skeleton();
    s.stageEntities    = static_cast<std::uint32_t>(m_stage.Entities().size());
    s.stageSpawns      = m_stage.Spawns();
    s.stageViewId      = m_stage.View();
    s.stageOriginY     = m_stage.OriginY();
    s.stagePosedFrames = m_stagePosed;
    s.stageRestFrames  = m_stageRest;
    s.stageStatus      = m_stageStatus;
    s.pinOwners        = renderer ? static_cast<std::uint32_t>(renderer->AnimPinOwnerCount()) : 0u;
    // The stage's clip and temporary pose
    s.clipPath       = m_clipPlayer.Path();
    s.clipSource     = m_clipPlayer.Source();
    s.clipName       = m_clipPlayer.ClipName();
    s.clipBound      = m_clipPlayer.Bound();
    s.clipPlaying    = m_clipPlayer.Playing();
    s.clipLooping    = m_clipPlayer.Looping();
    s.clipTime       = m_clipPlayer.Time();
    s.clipDuration   = m_clipPlayer.Duration();
    s.clipFps        = m_clipPlayer.Fps();
    s.clipSpeed      = m_clipPlayer.Speed();
    s.clipFrames     = m_clipPlayer.Frames();
    s.clipChannels   = m_clipPlayer.Channels();
    s.clipBoundBones = m_clipPlayer.BoundBones();
    s.drawnGhost     = m_lastOverlay.ghost;
    s.posedBones     = static_cast<std::uint32_t>(m_poseEdits.size());
    // The humanoid profile, the tab and the bone mask
    s.tab                = TabName(CurrentTab());
    s.humanoidDirty      = HumanoidDirty();
    s.humanoidUserEdited = Humanoid().userEdited != 0;
    s.humanoidConvention = Humanoid().convention;
    for (std::uint32_t r = 0; r < kHumanoidBoneCount; ++r)
        s.humanoidMissing += (m_skel && IsRequiredHumanoidBone(static_cast<HumanoidBone>(r)) &&
                              Humanoid().boneForRole[r] < 0) ? 1u : 0u;
    s.maskMode      = MaskModeName(m_maskMode);
    s.maskRoot      = m_maskRoot;
    s.maskResolved  = m_maskResolved;
    s.maskBones     = MaskBones();
    s.drawnHumanoid = m_lastOverlay.humanoid;
    s.drawnMasked   = m_lastOverlay.masked;
    s.selected     = m_selected;
    s.selectedName = (m_selected >= 0 && m_selected < static_cast<int>(m_names.size())) ? m_names[m_selected] : "";
    s.scope        = BoneScopeName(m_scope);
    s.show         = ShowNames(m_show);
    s.filter       = TreeFilterName(m_filter);
    s.search       = m_search;
    {
        // Counted with the current text (the tree's rows refresh on its next
        // draw), so a script sees its search at once.
        std::uint32_t rows = 0;
        const std::string lowered = LowerAscii(m_search);
        for (int b = 0; b < static_cast<int>(m_names.size()); ++b) rows += Matches(b, lowered) ? 1u : 0u;
        s.treeRows = rows;
    }
    s.drawnBones   = m_lastOverlay.bones;
    s.drawnJoints  = m_lastOverlay.joints;
    s.overlayLines = m_lastOverlay.lines;
    s.labels       = m_lastOverlay.labels;
    s.drawnSockets = m_lastOverlay.sockets;
    s.stats        = m_report.stats;
    s.errors       = m_report.Count(Resource::SkeletonIssueSeverity::Error);
    s.warnings     = m_report.Count(Resource::SkeletonIssueSeverity::Warning);
    s.infos        = m_report.Count(Resource::SkeletonIssueSeverity::Info);
    s.selectedSocket = SelectedSocket() >= 0 ? m_selSocket : std::string();
    s.socketLock     = SocketLock();
    s.gizmo          = GizmoOpName(m_gizmoOp);
    for (const Resource::ModelSocket& so : m_sockets) s.sockets += so.IsSkeletonSocket() ? 1u : 0u;
    s.socketsDirty   = SocketsDirty();
    s.undoDepth      = m_undo.size();
    s.redoDepth      = m_redo.size();
    s.gizmoGestures  = m_gizmoGestures;
    s.weightsOn       = m_weightsOn;
    s.weightsLive     = m_weights.Live();
    s.weightsBone     = m_weights.Bone();
    s.weightParts     = m_weights.Parts();
    s.weightSkipped   = m_weights.Skipped();
    s.weightsRevision = m_weights.Revision();
    s.animRows        = m_animRows.size();
    s.animPending     = m_animQueue.size();
    s.selectedVirtualBone = SelectedVirtualBone() >= 0 ? m_selVb : std::string();
    s.virtualBones        = static_cast<std::uint32_t>(m_vbs.size());
    for (const std::string& p : m_vbProblems) s.virtualBonesApplied += p.empty() ? 1u : 0u;
    s.drawnVirtualBones   = m_lastOverlay.virtualBones;
    s.virtualBonesDirty   = VirtualBonesDirty();
    s.dirty               = Dirty();
    s.staleInstances      = m_staleInstances;
    s.pickTargetFrom      = m_pickTargetFrom;
    return s;
}

// ==== Harness ====
bool SkeletonEditorWindow::HarnessConfigure(std::string& why)
{
    const auto& cl = Sys::CommandLine::Get();
    static const char* kFlags[] = {
        "-skeleton_editor_size", "-skeleton_editor_select", "-skeleton_editor_scope", "-skeleton_editor_show",
        "-skeleton_editor_orbit", "-skeleton_editor_shot", "-skeleton_editor_overlay_shot",
        "-skeleton_editor_close_at", "-skeleton_editor_overlay_off",
    };
    const bool hasOpen = cl.Has("-skeleton_editor");
    for (const char* f : kFlags)
        if (cl.Has(f) && !hasOpen)
        {
            why = std::string(f) + " without -skeleton_editor=<model.iscn | skeleton.iskel> has no subject";
            return false;
        }
    if (!hasOpen) return true;
    m_harnessOpen = Resource::AssetRefs::PercentDecode(cl.Get("-skeleton_editor"));
    if (m_harnessOpen.empty())
    {
        why = "-skeleton_editor needs <model.iscn | skeleton.iskel>";
        return false;
    }
    m_harnessOverlayOff = cl.Has("-skeleton_editor_overlay_off");
    if (cl.Has("-skeleton_editor_size"))
    {
        const std::string v = cl.Get("-skeleton_editor_size");
        unsigned w = 0, h = 0;
        if (sscanf_s(v.c_str(), "%ux%u", &w, &h) != 2 || w < 64 || h < 64 || w > 2048 || h > 2048)
        {
            why = "-skeleton_editor_size needs <w>x<h> (64..2048)";
            return false;
        }
        m_harnessW = w;
        m_harnessH = h;
    }
    auto list = [&](const char* flag, auto&& each) -> bool
    {
        if (!cl.Has(flag)) return true;
        const std::string v = cl.Get(flag);
        if (v.empty()) { why = std::string(flag) + " needs a value"; return false; }
        for (const std::string& item : SplitList(v, ','))
        {
            std::string value;
            std::uint64_t at = 0;
            if (!SplitAt(item, value, at)) { why = std::string(flag) + ": '" + item + "' is not <value>@<frame>"; return false; }
            if (!each(Resource::AssetRefs::PercentDecode(value), at))
            {
                if (why.empty()) why = std::string(flag) + ": bad value '" + value + "'";
                return false;
            }
        }
        return true;
    };
    auto push = [&](int kind, std::uint64_t at, std::string text, std::uint32_t value = 0)
    {
        Step s;
        s.kind  = kind;
        s.at    = at;
        s.text  = std::move(text);
        s.value = value;
        m_steps.push_back(std::move(s));
    };
    if (!list("-skeleton_editor_select", [&](const std::string& v, std::uint64_t at)
        { push(kStepSelect, at, v); return true; })) return false;
    if (!list("-skeleton_editor_scope", [&](const std::string& v, std::uint64_t at)
        {
            BoneScope sc;
            if (!ParseBoneScope(v, sc)) return false;
            push(kStepScope, at, v, static_cast<std::uint32_t>(sc));
            return true;
        })) return false;
    if (!list("-skeleton_editor_show", [&](const std::string& v, std::uint64_t at)
        {
            std::uint32_t bits = 0;
            if (!ParseShow(v, bits)) return false;
            push(kStepShow, at, v, bits);
            return true;
        })) return false;
    if (!list("-skeleton_editor_orbit", [&](const std::string& v, std::uint64_t at)
        {
            float y = 0.f, p = 0.f, d = 1.f, px = 0.f, py = 0.f, pz = 0.f;
            const int n = sscanf_s(v.c_str(), "%f:%f:%f:%f:%f:%f", &y, &p, &d, &px, &py, &pz);
            if (n != 3 && n != 6) return false;
            push(kStepOrbit, at, v);
            return true;
        })) return false;
    if (!list("-skeleton_editor_shot", [&](const std::string& v, std::uint64_t at)
        { push(kStepShot, at, HarnessPath(v)); return true; })) return false;
    if (!list("-skeleton_editor_overlay_shot", [&](const std::string& v, std::uint64_t at)
        { push(kStepOverlayShot, at, HarnessPath(v)); return true; })) return false;
    if (cl.Has("-skeleton_editor_close_at"))
    {
        const std::string v = cl.Get("-skeleton_editor_close_at");
        if (v.empty() || !std::all_of(v.begin(), v.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        {
            why = "-skeleton_editor_close_at needs an editor frame";
            return false;
        }
        push(kStepClose, std::strtoull(v.c_str(), nullptr, 10), "close");
    }
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
    LOG_INFO("[SKELEDITOR] harness: open='%s' steps=%zu size=%ux%u overlayOff=%d", m_harnessOpen.c_str(),
             m_steps.size(), m_harnessW, m_harnessH, m_harnessOverlayOff ? 1 : 0);
    return true;
}

void SkeletonEditorWindow::TickHarness(const SkeletonEditorContext& ctx)
{
    if (m_harnessOpen.empty() || m_harnessFailed) return;
    if (!m_harnessOpened)
    {
        m_harnessOpened = true;
        std::string code, why;
        if (!OpenNow(ctx, m_harnessOpen, code, why))
        {
            m_harnessFailed = true;
            LOG_ERROR("-skeleton_editor='%s' cannot open the Skeleton Editor (%s): %s", m_harnessOpen.c_str(),
                      code.c_str(), why.c_str());
            return;
        }
    }
    const std::uint64_t renders = (ctx.renderer && m_viewId != ~0u)
                                ? ctx.renderer->GetPreviewViewRenderCount(m_viewId) : 0;
    if (renders != m_lastRenders) { m_lastRenders = renders; m_lastRendersFrame = ctx.editorFrame; }
    bool settled = m_viewId != ~0u && renders > m_rendersAtDesc &&
                   ctx.editorFrame >= m_lastRendersFrame + kSettleFrames;

    for (Step& s : m_steps)
    {
        if (s.done || ctx.editorFrame < s.at) continue;
        if (s.kind == kStepOrbit) settled = false;
        switch (s.kind)
        {
        case kStepSelect:
        {
            bool ok = false;
            if (s.text == "none") ok = Select(-1);
            else if (!s.text.empty() && s.text[0] == '#')
                ok = Select(std::atoi(s.text.c_str() + 1));
            else ok = SelectByName(s.text);
            if (!ok)
            {
                m_harnessFailed = true;
                LOG_ERROR("-skeleton_editor_select: no bone '%s' in %s", s.text.c_str(), m_iskel.c_str());
            }
            else
                LOG_INFO("[SKELEDITOR] harness select -> %d '%s'", m_selected,
                         m_selected >= 0 ? m_names[m_selected].c_str() : "");
            s.done = true;
            break;
        }
        case kStepScope:
            SetScope(static_cast<BoneScope>(s.value));
            s.done = true;
            break;
        case kStepShow:
            SetShow(s.value);
            s.done = true;
            break;
        case kStepOrbit:
        {
            float y = 0.f, p = 0.f, d = 1.f, px = 0.f, py = 0.f, pz = 0.f;
            sscanf_s(s.text.c_str(), "%f:%f:%f:%f:%f:%f", &y, &p, &d, &px, &py, &pz);
            SetOrbit(y, p, d);
            SetPan(px, py, pz);
            s.done = true;
            break;
        }
        case kStepShot:
        case kStepOverlayShot:
        {
            if (!settled || !ctx.renderer)
            {
                if (ctx.editorFrame > s.at + kShotDeadline)
                {
                    s.done = true;
                    m_harnessFailed = true;
                    LOG_ERROR("[SKELEDITOR] shot '%s' at editor frame %llu: the view never settled (renders=%llu "
                              "sinceDesc=%llu)", s.text.c_str(), static_cast<unsigned long long>(s.at),
                              static_cast<unsigned long long>(renders),
                              static_cast<unsigned long long>(m_rendersAtDesc));
                }
                break;
            }
            s.done = true;
            if (s.kind == kStepShot)
            {
                const RHI::Texture* tex = ctx.renderer->GetPreviewOutputTexture(m_viewId);
                const bool ok = tex && ctx.gfx &&
                                ctx.gfx->CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE, s.text.c_str());
                if (!ok) m_harnessFailed = true;
                LOG_INFO("[SKELEDITOR] shot %s: file='%s' written=%d frame=%llu renders=%llu", BaseName(s.text).c_str(),
                         s.text.c_str(), ok ? 1 : 0, static_cast<unsigned long long>(ctx.editorFrame),
                         static_cast<unsigned long long>(renders));
            }
            else
            {
                std::string why;
                if (WriteOverlayPng(ctx.renderer, s.text, why) < 0)
                {
                    m_harnessFailed = true;
                    LOG_ERROR("-skeleton_editor_overlay_shot '%s' failed: %s", s.text.c_str(), why.c_str());
                }
            }
            break;
        }
        case kStepClose:
            Close();
            s.done = true;
            break;
        default:
            s.done = true;
            break;
        }
    }
}

} // namespace Editor
